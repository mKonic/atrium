#include "eis.hpp"

#include "input/keys.hpp"
#include "util/log.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"
#include "wl/seat.hpp"

#include <libeis.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <unordered_map>

namespace atrium {

// A client's seat and the devices made for what it bound.
struct Eis::Bound {
    eis_seat* seat = nullptr;
    uint32_t caps = 0;  // EIS_DEVICE_CAP_* bound
    std::vector<eis_device*> devices;
};

struct Eis::Session {
    Eis* eis = nullptr;
    uint64_t id = 0, owner = 0;
    uint32_t devices = 0;
    std::string path;
    ::eis* ctx = nullptr;
    wl_event_source* source = nullptr;
    std::vector<std::unique_ptr<Bound>> bound;
    // Its keys go through a keyboard of its own (its own Shift and Ctrl).
    std::unique_ptr<KeyboardGroup> keyboard;
    // Keysyms typed as keys: the key each is down on, and whether Shift went too.
    std::unordered_map<uint32_t, input::KeyFor> typed;
    // Touch ids, apart from the screen's own fingers.
    static constexpr int32_t kTouchBase = 1 << 20;

    // Input capture.
    CaptureListener listener;
    std::vector<input_capture::Barrier> barriers;
    bool enabled = false, active = false;
    bool meta = false;  // Super held while captured (Super+Escape takes it back)
    uint32_t activation = 0;
};

namespace {

uint32_t ms(eis_event* e) {
    return uint32_t(eis_event_get_time(e) / 1000);
}

std::string random_name() {
    std::random_device rd;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i)
        s += kHex[rd() & 15];
    return s;
}

} // namespace

Eis::Eis(Server& server) : server_(server) {}

Eis::~Eis() {
    while (!sessions_.empty())
        close(sessions_.back()->id);
}

std::optional<Eis::Opened> Eis::open(uint32_t devices, uint64_t owner, CaptureListener capture) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !(devices & (Keyboard | Pointer | Touch)))
        return std::nullopt;
    // In a directory only this user can enter, under a name no one guesses.
    const std::string dir = std::string(runtime) + "/atrium";
    mkdir(dir.c_str(), 0700);
    auto s = std::make_unique<Session>();
    s->id = next_++;
    s->owner = owner;
    s->devices = devices;
    s->listener = std::move(capture);
    s->path = dir + "/eis-" + random_name();
    s->ctx = eis_new(nullptr);
    if (!s->ctx || eis_setup_backend_socket(s->ctx, s->path.c_str()) != 0) {
        alog(Log::Error, "eis: can't listen on %s", s->path.c_str());
        if (s->ctx)
            eis_unref(s->ctx);
        return std::nullopt;
    }
    chmod(s->path.c_str(), 0600);
    Session* raw = s.get();
    raw->eis = this;
    s->source = wl_event_loop_add_fd(
        server_.loop, eis_get_fd(s->ctx), WL_EVENT_READABLE,
        [](int, uint32_t, void* data) {
            auto* session = static_cast<Session*>(data);
            session->eis->dispatch(*session);
            return 0;
        },
        raw);
    if ((devices & Keyboard) && !s->listener)
        s->keyboard = std::make_unique<KeyboardGroup>(*server_.seat, true);
    sessions_.push_back(std::move(s));
    return Opened{raw->id, raw->path};
}

bool Eis::close(uint64_t id) {
    auto it = std::ranges::find_if(sessions_, [id](const auto& s) { return s->id == id; });
    if (it == sessions_.end())
        return false;
    if ((*it)->active)
        deactivate(**it);
    std::unique_ptr<Session> s = std::move(*it);
    sessions_.erase(it);
    wl_event_source_remove(s->source);
    for (auto& b : s->bound) {
        drop_devices(*b);
        eis_seat_remove(b->seat);
        eis_seat_unref(b->seat);
    }
    eis_unref(s->ctx);
    unlink(s->path.c_str());
    // Fingers and keys it left down come up.
    for (auto it2 = server_.seat->touches_.begin(); it2 != server_.seat->touches_.end();)
        it2 = it2->first >= Session::kTouchBase ? server_.seat->touches_.erase(it2) : std::next(it2);
    if (s->keyboard && server_.seat->seat_kb_ == s->keyboard.get()) {
        s->keyboard.reset();
        server_.seat->use_keyboard(server_.seat->physical_keyboard());
    }
    return true;
}

void Eis::close_owned(uint64_t owner) {
    std::vector<uint64_t> ids;
    for (const auto& s : sessions_)
        if (s->owner == owner)
            ids.push_back(s->id);
    for (uint64_t id : ids)
        close(id);
}

void Eis::outputs_changed() {
    for (auto& s : sessions_)
        for (auto& b : s->bound)
            if (b->caps & (EIS_DEVICE_CAP_POINTER_ABSOLUTE | EIS_DEVICE_CAP_TOUCH)) {
                drop_devices(*b);
                make_devices(*s, *b);
            }
}

void Eis::drop_devices(Bound& b) {
    for (eis_device* d : b.devices) {
        eis_device_remove(d);
        eis_device_unref(d);
    }
    b.devices.clear();
}

void Eis::make_devices(Session& s, Bound& b) {
    auto device = [&](const char* name, std::initializer_list<eis_device_capability> caps, bool regions) {
        eis_device* d = eis_seat_new_device(b.seat);
        eis_device_configure_name(d, name);
        eis_device_configure_type(d, EIS_DEVICE_TYPE_VIRTUAL);
        for (auto c : caps)
            if (b.caps & c)
                eis_device_configure_capability(d, c);
        if (regions)
            for (const Output* o : server_.outputs) {
                if (o->dying || !o->enabled())
                    continue;
                eis_region* r = eis_device_new_region(d);
                eis_region_set_offset(r, uint32_t(std::max(0, o->box.x)), uint32_t(std::max(0, o->box.y)));
                eis_region_set_size(r, uint32_t(o->box.width), uint32_t(o->box.height));
                eis_region_add(r);
                eis_region_unref(r);
            }
        return d;
    };
    auto add = [&](eis_device* d) {
        eis_device_add(d);
        eis_device_resume(d);
        b.devices.push_back(d);
    };
    if (b.caps & EIS_DEVICE_CAP_POINTER)
        add(device("atrium remote pointer", {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL},
                   false));
    if (b.caps & EIS_DEVICE_CAP_POINTER_ABSOLUTE)
        add(device("atrium remote absolute pointer",
                   {EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL}, true));
    KeyboardGroup* keys = s.listener ? server_.seat->physical_keyboard() : s.keyboard.get();
    if ((b.caps & (EIS_DEVICE_CAP_KEYBOARD | EIS_DEVICE_CAP_TEXT)) && keys) {
        eis_device* d = device("atrium remote keyboard", {EIS_DEVICE_CAP_KEYBOARD, EIS_DEVICE_CAP_TEXT}, false);
        // Its keymap: the one its keys are read by (captured: the keyboard's own).
        if ((b.caps & EIS_DEVICE_CAP_KEYBOARD) && keys->keys.keymap()) {
            char* text = xkb_keymap_get_as_string(keys->keys.keymap(), XKB_KEYMAP_FORMAT_TEXT_V1);
            const size_t size = std::strlen(text) + 1;
            const int fd = memfd_create("atrium-eis-keymap", MFD_CLOEXEC);
            if (fd >= 0 && write(fd, text, size) == ssize_t(size)) {
                eis_keymap* km = eis_device_new_keymap(d, EIS_KEYMAP_TYPE_XKB, fd, size);
                eis_keymap_add(km);
                eis_keymap_unref(km);
            }
            if (fd >= 0)
                ::close(fd);
            free(text);
        }
        add(d);
    }
    if (b.caps & EIS_DEVICE_CAP_TOUCH)
        add(device("atrium remote touch", {EIS_DEVICE_CAP_TOUCH}, true));
}

void Eis::dispatch(Session& s) {
    Seat& seat = *server_.seat;
    eis_dispatch(s.ctx);
    while (eis_event* e = eis_get_event(s.ctx)) {
        switch (eis_event_get_type(e)) {
        case EIS_EVENT_CLIENT_CONNECT: {
            eis_client* client = eis_event_get_client(e);
            // Ones that send input, or for a capture ones that receive it.
            if (eis_client_is_sender(client) == bool(s.listener)) {
                eis_client_disconnect(client);
                break;
            }
            eis_client_connect(client);
            auto b = std::make_unique<Bound>();
            b->seat = eis_client_new_seat(client, "atrium");
            if (s.devices & Pointer) {
                eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_POINTER);
                if (!s.listener)
                    eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
                eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_BUTTON);
                eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_SCROLL);
            }
            if (s.devices & Keyboard) {
                eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_KEYBOARD);
                if (!s.listener)
                    eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_TEXT);
            }
            if ((s.devices & Touch) && !s.listener)
                eis_seat_configure_capability(b->seat, EIS_DEVICE_CAP_TOUCH);
            eis_seat_add(b->seat);
            s.bound.push_back(std::move(b));
            break;
        }
        case EIS_EVENT_CLIENT_DISCONNECT: {
            eis_client* client = eis_event_get_client(e);
            std::erase_if(s.bound, [&](const std::unique_ptr<Bound>& b) {
                if (eis_seat_get_client(b->seat) != client)
                    return false;
                drop_devices(*b);
                eis_seat_unref(b->seat);
                return true;
            });
            eis_client_disconnect(client);
            break;
        }
        case EIS_EVENT_SEAT_BIND: {
            eis_seat* es = eis_event_get_seat(e);
            for (auto& b : s.bound)
                if (b->seat == es) {
                    drop_devices(*b);
                    b->caps = 0;
                    for (auto c : {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_KEYBOARD,
                                   EIS_DEVICE_CAP_TOUCH, EIS_DEVICE_CAP_SCROLL, EIS_DEVICE_CAP_BUTTON,
                                   EIS_DEVICE_CAP_TEXT})
                        if (eis_event_seat_has_capability(e, c))
                            b->caps |= c;
                    make_devices(s, *b);
                }
            break;
        }
        case EIS_EVENT_DEVICE_CLOSED: {
            eis_device* d = eis_event_get_device(e);
            for (auto& b : s.bound)
                if (auto it = std::ranges::find(b->devices, d); it != b->devices.end()) {
                    eis_device_remove(d);
                    eis_device_unref(d);
                    b->devices.erase(it);
                }
            break;
        }
        case EIS_EVENT_POINTER_MOTION:
            seat.motion(ms(e), eis_event_pointer_get_dx(e), eis_event_pointer_get_dy(e), eis_event_pointer_get_dx(e),
                        eis_event_pointer_get_dy(e));
            break;
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
            seat.motion_absolute(ms(e), eis_event_pointer_get_absolute_x(e), eis_event_pointer_get_absolute_y(e));
            break;
        case EIS_EVENT_BUTTON_BUTTON:
            seat.button(ButtonEvent{ms(e), eis_event_button_get_button(e), eis_event_button_get_is_press(e)});
            break;
        case EIS_EVENT_SCROLL_DELTA:
            if (const double dy = eis_event_scroll_get_dy(e); dy != 0)
                seat.axis(AxisEvent{ms(e), WL_POINTER_AXIS_VERTICAL_SCROLL, dy, 0, WL_POINTER_AXIS_SOURCE_FINGER, false});
            if (const double dx = eis_event_scroll_get_dx(e); dx != 0)
                seat.axis(AxisEvent{ms(e), WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx, 0, WL_POINTER_AXIS_SOURCE_FINGER, false});
            break;
        case EIS_EVENT_SCROLL_STOP:
        case EIS_EVENT_SCROLL_CANCEL:
            if (eis_event_scroll_get_stop_y(e))
                seat.axis(AxisEvent{ms(e), WL_POINTER_AXIS_VERTICAL_SCROLL, 0, 0, WL_POINTER_AXIS_SOURCE_FINGER, false});
            if (eis_event_scroll_get_stop_x(e))
                seat.axis(AxisEvent{ms(e), WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0, 0, WL_POINTER_AXIS_SOURCE_FINGER, false});
            break;
        case EIS_EVENT_SCROLL_DISCRETE:
            // In 120ths of a wheel click; a click scrolls 15 pixels.
            if (const int32_t v = eis_event_scroll_get_discrete_dy(e); v != 0)
                seat.axis(AxisEvent{ms(e), WL_POINTER_AXIS_VERTICAL_SCROLL, v * 15.0 / 120, v,
                                    WL_POINTER_AXIS_SOURCE_WHEEL, false});
            if (const int32_t v = eis_event_scroll_get_discrete_dx(e); v != 0)
                seat.axis(AxisEvent{ms(e), WL_POINTER_AXIS_HORIZONTAL_SCROLL, v * 15.0 / 120, v,
                                    WL_POINTER_AXIS_SOURCE_WHEEL, false});
            break;
        case EIS_EVENT_KEYBOARD_KEY:
            if (s.keyboard)
                s.keyboard->keys.key(ms(e), eis_event_keyboard_get_key(e), eis_event_keyboard_get_key_is_press(e), true);
            break;
        case EIS_EVENT_TEXT_KEYSYM: {
            if (!s.keyboard)
                break;
            const uint32_t sym = eis_event_text_get_keysym(e);
            const bool press = eis_event_text_get_keysym_is_press(e);
            input::Keys& keys = s.keyboard->keys;
            if (press) {
                const auto k = input::key_for_keysym(keys.keymap(), keys.modifiers().group, sym);
                if (!k)
                    break;  // not on this keyboard
                s.typed[sym] = *k;
                if (k->shift)
                    keys.key(ms(e), KEY_LEFTSHIFT, true, true);
                keys.key(ms(e), k->keycode, true, true);
            } else if (auto it = s.typed.find(sym); it != s.typed.end()) {
                keys.key(ms(e), it->second.keycode, false, true);
                if (it->second.shift)
                    keys.key(ms(e), KEY_LEFTSHIFT, false, true);
                s.typed.erase(it);
            }
            break;
        }
        case EIS_EVENT_TOUCH_DOWN:
            seat.touch_down(ms(e), Session::kTouchBase + int32_t(eis_event_touch_get_id(e)), eis_event_touch_get_x(e),
                            eis_event_touch_get_y(e));
            break;
        case EIS_EVENT_TOUCH_MOTION:
            seat.touch_motion(ms(e), Session::kTouchBase + int32_t(eis_event_touch_get_id(e)),
                              eis_event_touch_get_x(e), eis_event_touch_get_y(e));
            break;
        case EIS_EVENT_TOUCH_UP:
            seat.touch_up(ms(e), Session::kTouchBase + int32_t(eis_event_touch_get_id(e)));
            break;
        case EIS_EVENT_FRAME: {
            eis_device* d = eis_event_get_device(e);
            if (eis_device_has_capability(d, EIS_DEVICE_CAP_TOUCH))
                server_.wl->seat->touch_frame();
            else if (!eis_device_has_capability(d, EIS_DEVICE_CAP_KEYBOARD))
                server_.wl->seat->pointer_frame();
            break;
        }
        default:
            break;
        }
        eis_event_unref(e);
    }
}

// --- input capture ------------------------------------------------------------

Eis::Session* Eis::find(uint64_t id) const {
    for (const auto& s : sessions_)
        if (s->id == id)
            return s.get();
    return nullptr;
}

Eis::Session* Eis::active() const {
    for (const auto& s : sessions_)
        if (s->active)
            return s.get();
    return nullptr;
}

std::vector<uint32_t> Eis::set_barriers(uint64_t id, std::vector<input_capture::Barrier> barriers) {
    std::vector<uint32_t> failed;
    Session* s = find(id);
    if (!s || !s->listener) {
        for (const auto& b : barriers)
            failed.push_back(b.id);
        return failed;
    }
    std::vector<Box> screens;
    for (const Output* o : server_.outputs)
        if (!o->dying && o->enabled())
            screens.push_back(o->box);
    s->barriers.clear();
    for (const auto& b : barriers) {
        if (input_capture::valid(b, screens))
            s->barriers.push_back(b);
        else
            failed.push_back(b.id);
    }
    // New barriers wait for Enable again.
    if (s->active)
        deactivate(*s);
    s->enabled = false;
    return failed;
}

bool Eis::enable(uint64_t id) {
    Session* s = find(id);
    if (!s || !s->listener)
        return false;
    s->enabled = true;
    return true;
}

bool Eis::disable(uint64_t id) {
    Session* s = find(id);
    if (!s || !s->listener)
        return false;
    if (s->active)
        deactivate(*s);
    s->enabled = false;
    return true;
}

bool Eis::release(uint64_t id, std::optional<std::pair<double, double>> to) {
    Session* s = find(id);
    if (!s || !s->active)
        return false;
    if (to)
        server_.seat->cursor->warp_closest(to->first, to->second);
    deactivate(*s);
    return true;
}

void Eis::to_devices(Session& s, int cap, const std::function<void(eis_device*)>& send) {
    for (auto& b : s.bound)
        for (eis_device* d : b->devices)
            if (eis_device_has_capability(d, eis_device_capability(cap))) {
                send(d);
                eis_device_frame(d, eis_now(s.ctx));
            }
}

void Eis::activate(Session& s, double x, double y, uint32_t barrier) {
    s.active = true;
    s.meta = false;
    s.activation += 1;
    for (auto& b : s.bound)
        for (eis_device* d : b->devices)
            eis_device_start_emulating(d, s.activation);
    // The pointer is on the other computer now: not shown here.
    server_.seat->cursor->unset_image();
    if (s.listener)
        s.listener({CaptureEvent::Activated, s.activation, x, y, barrier});
}

void Eis::deactivate(Session& s) {
    s.active = false;
    for (auto& b : s.bound)
        for (eis_device* d : b->devices)
            eis_device_stop_emulating(d);
    // Back here: shown again, over whatever it's on.
    Seat& seat = *server_.seat;
    seat.cursor->set_xcursor(seat.xcursor.get(), "default");
    seat.motion(0, 0, 0, 0, 0);
    if (s.listener)
        s.listener({CaptureEvent::Deactivated, s.activation, seat.cursor->x, seat.cursor->y, 0});
}

bool Eis::capture_motion(uint32_t, double x, double y, double dx, double dy) {
    if (Session* a = active()) {
        to_devices(*a, EIS_DEVICE_CAP_POINTER, [&](eis_device* d) { eis_device_pointer_motion(d, dx, dy); });
        return true;
    }
    for (auto& s : sessions_) {
        if (!s->enabled || s->barriers.empty())
            continue;
        if (const auto c = input_capture::crossing(s->barriers, x, y, dx, dy)) {
            // The pointer waits at the edge it went through.
            server_.seat->cursor->warp_closest(c->x, c->y);
            activate(*s, c->x, c->y, c->id);
            return true;
        }
    }
    return false;
}

bool Eis::capture_button(uint32_t, uint32_t button, bool pressed) {
    Session* a = active();
    if (!a)
        return false;
    to_devices(*a, EIS_DEVICE_CAP_BUTTON, [&](eis_device* d) { eis_device_button_button(d, button, pressed); });
    return true;
}

bool Eis::capture_axis(uint32_t, uint32_t orientation, double delta, int32_t value120) {
    Session* a = active();
    if (!a)
        return false;
    const bool vertical = orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
    to_devices(*a, EIS_DEVICE_CAP_SCROLL, [&](eis_device* d) {
        if (value120)
            eis_device_scroll_discrete(d, vertical ? 0 : value120, vertical ? value120 : 0);
        else if (delta != 0)
            eis_device_scroll_delta(d, vertical ? 0 : delta, vertical ? delta : 0);
        else
            eis_device_scroll_stop(d, !vertical, vertical);
    });
    return true;
}

bool Eis::capture_key(uint32_t, uint32_t keycode, bool pressed) {
    Session* a = active();
    if (!a)
        return false;
    if (keycode == KEY_LEFTMETA || keycode == KEY_RIGHTMETA)
        a->meta = pressed;
    // Super+Escape: the keyboard and pointer come back, and stay (whatever
    // the app does).
    if (keycode == KEY_ESC && pressed && a->meta) {
        to_devices(*a, EIS_DEVICE_CAP_KEYBOARD, [&](eis_device* d) {
            eis_device_keyboard_key(d, KEY_LEFTMETA, false);
            eis_device_keyboard_key(d, KEY_RIGHTMETA, false);
        });
        deactivate(*a);
        a->enabled = false;
        if (a->listener)
            a->listener({CaptureEvent::Disabled, a->activation, 0, 0, 0});
        return true;
    }
    to_devices(*a, EIS_DEVICE_CAP_KEYBOARD, [&](eis_device* d) { eis_device_keyboard_key(d, keycode, pressed); });
    return true;
}

} // namespace atrium
