#include "eis.hpp"

#include "ipc.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"

#include <libeis.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <set>

namespace atrium {

namespace {

const wlr_pointer_impl kPointer{.name = "eis-pointer"};
const wlr_keyboard_impl kKeyboard{.name = "eis-keyboard", .led_update = nullptr};
const wlr_touch_impl kTouch{.name = "eis-touch"};

uint32_t now_ms() {
    using namespace std::chrono;
    return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

struct Eis::Session {
    uint32_t cookie = 0;
    const void* owner = nullptr;
    bool capture = false;
    uint32_t devices = 0;  // the portal's bits

    ::eis* ctx = nullptr;
    wl_event_source* source = nullptr;
    eis_client* client = nullptr;
    eis_seat* seat = nullptr;
    // What the client bound, and the devices it was given.
    bool want_pointer = false, want_absolute = false, want_keyboard = false;
    eis_device* pointer = nullptr;
    eis_device* absolute = nullptr;
    eis_device* keyboard = nullptr;

    // Remote control: the devices atrium's seat sees.
    std::unique_ptr<wlr_pointer> vpointer;
    std::unique_ptr<wlr_keyboard> vkeyboard;
    std::unique_ptr<wlr_touch> vtouch;
    std::map<uint32_t, std::pair<uint32_t, bool>> keysyms;  // held: keycode, with Shift
    std::set<uint32_t> buttons;

    // Input capture.
    std::vector<eis::Barrier> barriers;
    bool enabled = false;
    uint32_t activation = 0;
};

Eis::Eis(Server& server) : server_(server) {}

Eis::~Eis() {
    while (!sessions_.empty())
        close(sessions_.begin()->first);
}

Eis::Session* Eis::find(uint32_t cookie) const {
    const auto it = sessions_.find(cookie);
    return it == sessions_.end() ? nullptr : it->second.get();
}

uint32_t Eis::remote_start(const void* owner, uint32_t devices) {
    auto s = std::make_unique<Session>();
    s->cookie = ++next_cookie_;
    s->owner = owner;
    s->devices = devices;
    ensure_virtual(*s);
    const uint32_t cookie = s->cookie;
    sessions_.emplace(cookie, std::move(s));
    return cookie;
}

uint32_t Eis::capture_create(const void* owner, uint32_t devices) {
    auto s = std::make_unique<Session>();
    s->cookie = ++next_cookie_;
    s->owner = owner;
    s->devices = devices;
    s->capture = true;
    const uint32_t cookie = s->cookie;
    sessions_.emplace(cookie, std::move(s));
    return cookie;
}

void Eis::ensure_virtual(Session& s) {
    // Made once, for the session's life: atrium's seat takes them as it would
    // anything plugged in.
    if ((s.devices & eis::Pointer) && !s.vpointer) {
        s.vpointer = std::make_unique<wlr_pointer>();
        wlr_pointer_init(s.vpointer.get(), &kPointer, "remote pointer");
        server_.seat->add_virtual(&s.vpointer->base);
    }
    if ((s.devices & eis::Keyboard) && !s.vkeyboard) {
        s.vkeyboard = std::make_unique<wlr_keyboard>();
        wlr_keyboard_init(s.vkeyboard.get(), &kKeyboard, "remote keyboard");
        server_.seat->add_virtual(&s.vkeyboard->base);
    }
    if ((s.devices & eis::Touchscreen) && !s.vtouch) {
        s.vtouch = std::make_unique<wlr_touch>();
        wlr_touch_init(s.vtouch.get(), &kTouch, "remote touchscreen");
        server_.seat->add_virtual(&s.vtouch->base);
    }
}

void Eis::close(uint32_t cookie) {
    const auto it = sessions_.find(cookie);
    if (it == sessions_.end())
        return;
    Session& s = *it->second;
    if (active_ == &s)
        deactivate(std::nullopt);
    // Let go of whatever it still holds.
    if (s.vkeyboard) {
        for (const auto& [sym, held] : s.keysyms) {
            wlr_keyboard_key_event e{.time_msec = now_ms(), .keycode = held.first, .update_state = true,
                                     .state = WL_KEYBOARD_KEY_STATE_RELEASED};
            wlr_keyboard_notify_key(s.vkeyboard.get(), &e);
        }
        wlr_keyboard_finish(s.vkeyboard.get());
    }
    if (s.vpointer) {
        for (uint32_t b : s.buttons) {
            wlr_pointer_button_event e{.pointer = s.vpointer.get(), .time_msec = now_ms(), .button = b,
                                       .state = WL_POINTER_BUTTON_STATE_RELEASED};
            wlr_pointer_notify_button(s.vpointer.get(), &e);
        }
        wlr_pointer_finish(s.vpointer.get());
    }
    if (s.vtouch)
        wlr_touch_finish(s.vtouch.get());
    remove_devices(s);
    if (s.seat)
        eis_seat_unref(s.seat);
    if (s.client) {
        eis_client_disconnect(s.client);
        eis_client_unref(s.client);
    }
    if (s.source)
        wl_event_source_remove(s.source);
    if (s.ctx)
        eis_unref(s.ctx);
    sessions_.erase(it);
}

void Eis::drop_owner(const void* owner) {
    std::vector<uint32_t> gone;
    for (const auto& [cookie, s] : sessions_)
        if (s->owner == owner)
            gone.push_back(cookie);
    for (uint32_t c : gone)
        close(c);
}

// --- libei -------------------------------------------------------------------------

int Eis::connect(uint32_t cookie) {
    Session* s = find(cookie);
    if (!s)
        return -1;
    if (!s->ctx) {
        s->ctx = eis_new(this);
        if (!s->ctx || eis_setup_backend_fd(s->ctx) != 0)
            return -1;
        struct Ctx {
            Eis* self;
            uint32_t cookie;
        };
        s->source = wl_event_loop_add_fd(server_.loop, eis_get_fd(s->ctx), WL_EVENT_READABLE,
            [](int, uint32_t, void* data) {
                auto* self = static_cast<Eis*>(data);
                // Every session's context: cheap, and a session may close
                // while its events are handled.
                std::vector<uint32_t> cookies;
                for (const auto& [c, s] : self->sessions_)
                    if (s->ctx)
                        cookies.push_back(c);
                for (uint32_t c : cookies)
                    if (Session* s = self->find(c))
                        self->dispatch(*s);
                return 0;
            }, this);
    }
    return eis_backend_fd_add_client(s->ctx);
}

void Eis::dispatch(Session& s) {
    eis_dispatch(s.ctx);
    const uint32_t cookie = s.cookie;
    while (eis_event* e = eis_get_event(s.ctx)) {
        handle(s, e);
        eis_event_unref(e);
        if (!find(cookie))
            return;
    }
}

void Eis::handle(Session& s, eis_event* e) {
    switch (eis_event_get_type(e)) {
    case EIS_EVENT_CLIENT_CONNECT: {
        eis_client* client = eis_event_get_client(e);
        // Remote control takes a client that sends input, capture one that
        // receives it; one each.
        if (eis_client_is_sender(client) == s.capture || s.client) {
            eis_client_disconnect(client);
            return;
        }
        eis_client_connect(client);
        s.client = eis_client_ref(client);
        s.seat = eis_client_new_seat(client, "atrium");
        if (s.devices & eis::Pointer) {
            eis_seat_configure_capability(s.seat, EIS_DEVICE_CAP_POINTER);
            eis_seat_configure_capability(s.seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_seat_configure_capability(s.seat, EIS_DEVICE_CAP_BUTTON);
            eis_seat_configure_capability(s.seat, EIS_DEVICE_CAP_SCROLL);
        }
        if (s.devices & eis::Keyboard)
            eis_seat_configure_capability(s.seat, EIS_DEVICE_CAP_KEYBOARD);
        if (s.devices & eis::Touchscreen)
            eis_seat_configure_capability(s.seat, EIS_DEVICE_CAP_TOUCH);
        eis_seat_add(s.seat);
        return;
    }
    case EIS_EVENT_CLIENT_DISCONNECT:
        if (eis_event_get_client(e) != s.client)
            return;
        remove_devices(s);
        if (s.seat)
            eis_seat_unref(std::exchange(s.seat, nullptr));
        eis_client_unref(std::exchange(s.client, nullptr));
        return;
    case EIS_EVENT_SEAT_BIND:
        bind_seat(s, e);
        return;
    case EIS_EVENT_DEVICE_CLOSED: {
        eis_device* d = eis_event_get_device(e);
        for (eis_device** mine : {&s.pointer, &s.absolute, &s.keyboard})
            if (*mine == d) {
                eis_device_remove(d);
                eis_device_unref(d);
                *mine = nullptr;
            }
        return;
    }
    default:
        break;
    }
    if (s.capture)
        return;  // a receiving client has nothing to send

    // Remote control: what the client sends goes to the session's devices.
    const uint32_t t = now_ms();
    wlr_pointer* p = s.vpointer.get();
    switch (eis_event_get_type(e)) {
    case EIS_EVENT_POINTER_MOTION:
        if (p) {
            const double dx = eis_event_pointer_get_dx(e), dy = eis_event_pointer_get_dy(e);
            wlr_pointer_motion_event m{.pointer = p, .time_msec = t, .delta_x = dx, .delta_y = dy,
                                       .unaccel_dx = dx, .unaccel_dy = dy};
            wl_signal_emit_mutable(&p->events.motion, &m);
        }
        break;
    case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
        if (p) {
            wlr_pointer_motion_absolute_event m{.pointer = p, .time_msec = t};
            normalize(eis_event_pointer_get_absolute_x(e), eis_event_pointer_get_absolute_y(e), m.x, m.y);
            wl_signal_emit_mutable(&p->events.motion_absolute, &m);
        }
        break;
    case EIS_EVENT_BUTTON_BUTTON:
        if (p) {
            const uint32_t b = eis_event_button_get_button(e);
            const bool press = eis_event_button_get_is_press(e);
            if (press ? !s.buttons.insert(b).second : !s.buttons.erase(b))
                break;  // pressed twice, or let go of unpressed
            wlr_pointer_button_event m{.pointer = p, .time_msec = t, .button = b,
                                       .state = press ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
            wlr_pointer_notify_button(p, &m);
        }
        break;
    case EIS_EVENT_SCROLL_DELTA:
    case EIS_EVENT_SCROLL_DISCRETE:
        if (p) {
            const bool discrete = eis_event_get_type(e) == EIS_EVENT_SCROLL_DISCRETE;
            const double dx = discrete ? eis_event_scroll_get_discrete_dx(e) : eis_event_scroll_get_dx(e);
            const double dy = discrete ? eis_event_scroll_get_discrete_dy(e) : eis_event_scroll_get_dy(e);
            for (int i = 0; i < 2; i++) {
                const double v = i == 0 ? dx : dy;
                if (v == 0)
                    continue;
                // A notch (120) scrolls 15, as libinput's wheel does.
                wlr_pointer_axis_event a{.pointer = p, .time_msec = t,
                    .source = discrete ? WL_POINTER_AXIS_SOURCE_WHEEL : WL_POINTER_AXIS_SOURCE_CONTINUOUS,
                    .orientation = i == 0 ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
                    .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
                    .delta = discrete ? v * 15 / 120 : v, .delta_discrete = discrete ? int32_t(v) : 0};
                wl_signal_emit_mutable(&p->events.axis, &a);
            }
        }
        break;
    case EIS_EVENT_SCROLL_STOP:
    case EIS_EVENT_SCROLL_CANCEL:
        if (p)
            for (int i = 0; i < 2; i++) {
                if (!(i == 0 ? eis_event_scroll_get_stop_x(e) : eis_event_scroll_get_stop_y(e)))
                    continue;
                wlr_pointer_axis_event a{.pointer = p, .time_msec = t, .source = WL_POINTER_AXIS_SOURCE_CONTINUOUS,
                    .orientation = i == 0 ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
                    .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL, .delta = 0, .delta_discrete = 0};
                wl_signal_emit_mutable(&p->events.axis, &a);
            }
        break;
    case EIS_EVENT_FRAME:
        if (p && (eis_event_get_device(e) == s.pointer || eis_event_get_device(e) == s.absolute))
            wl_signal_emit_mutable(&p->events.frame, p);
        if (s.vtouch && eis_event_get_device(e) == s.absolute)
            wl_signal_emit_mutable(&s.vtouch->events.frame, nullptr);
        break;
    case EIS_EVENT_KEYBOARD_KEY:
        if (s.vkeyboard) {
            wlr_keyboard_key_event k{.time_msec = t, .keycode = eis_event_keyboard_get_key(e), .update_state = true,
                .state = eis_event_keyboard_get_key_is_press(e) ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
            wlr_keyboard_notify_key(s.vkeyboard.get(), &k);
        }
        break;
    case EIS_EVENT_TOUCH_DOWN:
    case EIS_EVENT_TOUCH_MOTION:
        if (s.vtouch) {
            double x, y;
            normalize(eis_event_touch_get_x(e), eis_event_touch_get_y(e), x, y);
            const int32_t id = int32_t(eis_event_touch_get_id(e));
            if (eis_event_get_type(e) == EIS_EVENT_TOUCH_DOWN) {
                wlr_touch_down_event d{.touch = s.vtouch.get(), .time_msec = t, .touch_id = id, .x = x, .y = y};
                wl_signal_emit_mutable(&s.vtouch->events.down, &d);
            } else {
                wlr_touch_motion_event m{.touch = s.vtouch.get(), .time_msec = t, .touch_id = id, .x = x, .y = y};
                wl_signal_emit_mutable(&s.vtouch->events.motion, &m);
            }
        }
        break;
    case EIS_EVENT_TOUCH_UP:
        if (s.vtouch) {
            wlr_touch_up_event u{.touch = s.vtouch.get(), .time_msec = t, .touch_id = int32_t(eis_event_touch_get_id(e))};
            wl_signal_emit_mutable(&s.vtouch->events.up, &u);
        }
        break;
    default:
        break;
    }
}

void Eis::bind_seat(Session& s, eis_event* e) {
    s.want_pointer = eis_event_seat_has_capability(e, EIS_DEVICE_CAP_POINTER);
    s.want_absolute = eis_event_seat_has_capability(e, EIS_DEVICE_CAP_POINTER_ABSOLUTE) ||
                      eis_event_seat_has_capability(e, EIS_DEVICE_CAP_TOUCH);
    s.want_keyboard = eis_event_seat_has_capability(e, EIS_DEVICE_CAP_KEYBOARD);
    remove_devices(s);
    add_devices(s);
}

eis_device* Eis::new_device(Session& s, const char* name) {
    eis_device* d = eis_seat_new_device(s.seat);
    eis_device_configure_name(d, name);
    return d;
}

eis_device* Eis::absolute_device(Session& s) {
    eis_device* d = new_device(s, "atrium absolute pointer");
    if (s.devices & eis::Pointer) {
        eis_device_configure_capability(d, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
        eis_device_configure_capability(d, EIS_DEVICE_CAP_BUTTON);
        eis_device_configure_capability(d, EIS_DEVICE_CAP_SCROLL);
    }
    if (s.devices & eis::Touchscreen)
        eis_device_configure_capability(d, EIS_DEVICE_CAP_TOUCH);
    // A region per screen, in layout coordinates, named as the screen cast
    // names its stream (mapping_id).
    for (Output* o : server_.outputs) {
        if (!o->enabled())
            continue;
        wlr_box box;
        wlr_output_layout_get_box(server_.output_layout, o->wlr, &box);
        eis_region* r = eis_device_new_region(d);
        eis_region_set_offset(r, uint32_t(box.x), uint32_t(box.y));
        eis_region_set_size(r, uint32_t(box.width), uint32_t(box.height));
        eis_region_set_physical_scale(r, o->wlr->scale);
        eis_region_set_mapping_id(r, o->wlr->name);
        eis_region_add(r);
        eis_region_unref(r);
    }
    return d;
}

int Eis::keymap_fd(size_t& size) const {
    wlr_keyboard* kb = server_.seat->physical_keyboard();
    char* text = kb && kb->keymap ? xkb_keymap_get_as_string(kb->keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
    if (!text)
        return -1;
    size = std::strlen(text) + 1;
    const int fd = memfd_create("atrium-eis-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0 && write(fd, text, size) == ssize_t(size))
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    std::free(text);
    return fd;
}

eis_device* Eis::keyboard_device(Session& s) {
    eis_device* d = new_device(s, "atrium keyboard");
    eis_device_configure_capability(d, EIS_DEVICE_CAP_KEYBOARD);
    size_t size = 0;
    const int fd = keymap_fd(size);
    if (fd >= 0) {
        eis_keymap* k = eis_device_new_keymap(d, EIS_KEYMAP_TYPE_XKB, fd, size);
        eis_keymap_add(k);
        eis_keymap_unref(k);
        ::close(fd);  // libeis keeps its own
    }
    return d;
}

void Eis::add_devices(Session& s) {
    if (!s.seat)
        return;
    auto start = [this, &s](eis_device* d) {
        eis_device_add(d);
        eis_device_resume(d);
        // A capture's devices send while it holds the input.
        if (active_ == &s)
            eis_device_start_emulating(d, s.activation);
        return d;
    };
    if (s.want_pointer && (s.devices & eis::Pointer)) {
        eis_device* d = new_device(s, "atrium pointer");
        eis_device_configure_capability(d, EIS_DEVICE_CAP_POINTER);
        eis_device_configure_capability(d, EIS_DEVICE_CAP_BUTTON);
        eis_device_configure_capability(d, EIS_DEVICE_CAP_SCROLL);
        s.pointer = start(d);
    }
    if (s.want_absolute && (s.devices & (eis::Pointer | eis::Touchscreen)))
        s.absolute = start(absolute_device(s));
    if (s.want_keyboard && (s.devices & eis::Keyboard))
        s.keyboard = start(keyboard_device(s));
}

void Eis::remove_devices(Session& s) {
    for (eis_device** d : {&s.pointer, &s.absolute, &s.keyboard})
        if (*d) {
            eis_device_remove(*d);
            eis_device_unref(std::exchange(*d, nullptr));
        }
}

void Eis::outputs_changed() {
    for (auto& [c, s] : sessions_)
        if (s->absolute) {
            eis_device_remove(s->absolute);
            eis_device_unref(std::exchange(s->absolute, nullptr));
            if (s->want_absolute) {
                s->absolute = absolute_device(*s);
                eis_device_add(s->absolute);
                eis_device_resume(s->absolute);
            }
        }
}

void Eis::keymap_changed() {
    for (auto& [c, s] : sessions_)
        if (s->keyboard) {
            eis_device_remove(s->keyboard);
            eis_device_unref(std::exchange(s->keyboard, nullptr));
            s->keyboard = keyboard_device(*s);
            eis_device_add(s->keyboard);
            eis_device_resume(s->keyboard);
        }
}

void Eis::normalize(double lx, double ly, double& nx, double& ny) const {
    wlr_box box;
    wlr_output_layout_get_box(server_.output_layout, nullptr, &box);
    nx = box.width > 0 ? (lx - box.x) / box.width : 0;
    ny = box.height > 0 ? (ly - box.y) / box.height : 0;
}

uint64_t Eis::now_us() const {
    using namespace std::chrono;
    return uint64_t(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// --- the portal's Notify* ----------------------------------------------------------

bool Eis::remote_input(uint32_t cookie, const nlohmann::json& ev) {
    Session* s = find(cookie);
    if (!s || s->capture)
        return false;
    const std::string type = ev.value("type", "");
    const uint32_t t = now_ms();
    wlr_pointer* p = s->vpointer.get();
    if (type == "motion" || type == "absolute" || type == "button" || type == "axis" || type == "axis_discrete") {
        if (!p)
            return false;
        if (type == "motion") {
            const double dx = ev.value("dx", 0.0), dy = ev.value("dy", 0.0);
            wlr_pointer_motion_event m{.pointer = p, .time_msec = t, .delta_x = dx, .delta_y = dy,
                                       .unaccel_dx = dx, .unaccel_dy = dy};
            wl_signal_emit_mutable(&p->events.motion, &m);
        } else if (type == "absolute") {
            wlr_pointer_motion_absolute_event m{.pointer = p, .time_msec = t};
            normalize(ev.value("x", 0.0), ev.value("y", 0.0), m.x, m.y);
            wl_signal_emit_mutable(&p->events.motion_absolute, &m);
        } else if (type == "button") {
            const uint32_t b = ev.value("button", 0u);
            const bool press = ev.value("pressed", false);
            if (press ? !s->buttons.insert(b).second : !s->buttons.erase(b))
                return true;
            wlr_pointer_button_event m{.pointer = p, .time_msec = t, .button = b,
                .state = press ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED};
            wlr_pointer_notify_button(p, &m);
        } else {
            const bool discrete = type == "axis_discrete";
            // axis_discrete: {axis: 0 vertical / 1 horizontal, steps}.
            const double dx = discrete ? (ev.value("axis", 0) == 1 ? ev.value("steps", 0) : 0) : ev.value("dx", 0.0);
            const double dy = discrete ? (ev.value("axis", 0) == 0 ? ev.value("steps", 0) : 0) : ev.value("dy", 0.0);
            const bool finish = !discrete && ev.value("finish", false);
            for (int i = 0; i < 2; i++) {
                const double v = i == 0 ? dx : dy;
                if (v == 0 && !finish)
                    continue;
                wlr_pointer_axis_event a{.pointer = p, .time_msec = t,
                    .source = discrete ? WL_POINTER_AXIS_SOURCE_WHEEL : WL_POINTER_AXIS_SOURCE_FINGER,
                    .orientation = i == 0 ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
                    .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
                    .delta = discrete ? v * 15 : v, .delta_discrete = discrete ? int32_t(v * 120) : 0};
                wl_signal_emit_mutable(&p->events.axis, &a);
            }
        }
        wl_signal_emit_mutable(&p->events.frame, p);
        return true;
    }
    if (type == "key" || type == "keysym") {
        wlr_keyboard* kb = s->vkeyboard.get();
        if (!kb)
            return false;
        const bool press = ev.value("pressed", false);
        auto send = [kb, t](uint32_t keycode, bool down) {
            wlr_keyboard_key_event k{.time_msec = t, .keycode = keycode, .update_state = true,
                                     .state = down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
            wlr_keyboard_notify_key(kb, &k);
        };
        if (type == "key") {
            send(ev.value("keycode", 0u), press);
            return true;
        }
        // A keysym: the key that types it in the layout in use, with Shift
        // when it's on the shifted level.
        const uint32_t sym = ev.value("keysym", 0u);
        if (!press) {
            const auto it = s->keysyms.find(sym);
            if (it == s->keysyms.end())
                return true;
            send(it->second.first, false);
            if (it->second.second)
                send(KEY_LEFTSHIFT, false);
            s->keysyms.erase(it);
            return true;
        }
        xkb_keymap* keymap = kb->keymap;
        const xkb_layout_index_t layout = kb->xkb_state ? xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE) : 0;
        for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap); code <= xkb_keymap_max_keycode(keymap); code++)
            for (xkb_level_index_t level = 0; level < 2; level++) {
                const xkb_keysym_t* syms = nullptr;
                const int n = xkb_keymap_key_get_syms_by_level(keymap, code, layout, level, &syms);
                if (n != 1 || syms[0] != sym)
                    continue;
                if (level == 1)
                    send(KEY_LEFTSHIFT, true);
                send(code - 8, true);
                s->keysyms[sym] = {code - 8, level == 1};
                return true;
            }
        return false;
    }
    if (type == "touch_down" || type == "touch_motion" || type == "touch_up") {
        wlr_touch* touch = s->vtouch.get();
        if (!touch)
            return false;
        const int32_t id = ev.value("slot", 0);
        if (type == "touch_up") {
            wlr_touch_up_event u{.touch = touch, .time_msec = t, .touch_id = id};
            wl_signal_emit_mutable(&touch->events.up, &u);
        } else {
            double x, y;
            normalize(ev.value("x", 0.0), ev.value("y", 0.0), x, y);
            if (type == "touch_down") {
                wlr_touch_down_event d{.touch = touch, .time_msec = t, .touch_id = id, .x = x, .y = y};
                wl_signal_emit_mutable(&touch->events.down, &d);
            } else {
                wlr_touch_motion_event m{.touch = touch, .time_msec = t, .touch_id = id, .x = x, .y = y};
                wl_signal_emit_mutable(&touch->events.motion, &m);
            }
        }
        wl_signal_emit_mutable(&touch->events.frame, nullptr);
        return true;
    }
    return false;
}

// --- input capture -------------------------------------------------------------------

bool Eis::capture_barriers(uint32_t cookie, const std::vector<eis::Barrier>& barriers) {
    Session* s = find(cookie);
    if (!s || !s->capture)
        return false;
    s->barriers = barriers;
    return true;
}

bool Eis::capture_enable(uint32_t cookie, bool on) {
    Session* s = find(cookie);
    if (!s || !s->capture)
        return false;
    if (!on && active_ == s)
        deactivate(std::nullopt);
    s->enabled = on;
    return true;
}

bool Eis::capture_release(uint32_t cookie, std::optional<std::pair<double, double>> at) {
    Session* s = find(cookie);
    if (!s || active_ != s)
        return false;
    deactivate(at);
    return true;
}

void Eis::activate(Session& s, uint32_t barrier, double x, double y) {
    active_ = &s;
    s.activation++;
    for (eis_device* d : {s.pointer, s.absolute, s.keyboard})
        if (d)
            eis_device_start_emulating(d, s.activation);
    if (s.keyboard) {
        const wlr_keyboard* kb = server_.seat->physical_keyboard();
        eis_device_keyboard_send_xkb_modifiers(s.keyboard, kb->modifiers.depressed, kb->modifiers.latched,
                                               kb->modifiers.locked, kb->modifiers.group);
    }
    // The pointer stays where it went out, out of sight.
    wlr_cursor_unset_image(server_.seat->cursor);
    wlr_seat_pointer_notify_clear_focus(server_.seat->wlr);
    if (server_.ipc)
        server_.ipc->broadcast("portal", {{"event", "capture.activated"}, {"cookie", s.cookie},
            {"activation_id", s.activation}, {"barrier", barrier}, {"x", x}, {"y", y}});
}

void Eis::deactivate(std::optional<std::pair<double, double>> at) {
    Session* s = std::exchange(active_, nullptr);
    if (!s)
        return;
    for (eis_device* d : {s->pointer, s->absolute, s->keyboard})
        if (d)
            eis_device_stop_emulating(d);
    if (at)
        wlr_cursor_warp(server_.seat->cursor, nullptr, at->first, at->second);
    server_.seat->set_default_cursor();
    server_.seat->refresh_pointer();
    if (server_.ipc)
        server_.ipc->broadcast("portal", {{"event", "capture.deactivated"}, {"cookie", s->cookie},
            {"activation_id", s->activation}, {"x", server_.seat->cursor->x}, {"y", server_.seat->cursor->y}});
}

bool Eis::motion(double x, double y, double dx, double dy, double udx, double udy) {
    if (active_) {
        if (active_->pointer) {
            eis_device_pointer_motion(active_->pointer, udx, udy);
            eis_device_frame(active_->pointer, now_us());
        }
        return true;
    }
    for (auto& [c, s] : sessions_) {
        if (!s->capture || !s->enabled || !s->client)
            continue;
        if (const auto barrier = eis::crossed(s->barriers, x, y, dx, dy)) {
            activate(*s, *barrier, x + dx, y + dy);  // where it would have gone, as KWin reports it
            return true;
        }
    }
    return false;
}

bool Eis::button(uint32_t button, bool pressed) {
    if (!active_)
        return false;
    if (eis_device* d = active_->pointer ? active_->pointer : active_->absolute) {
        eis_device_button_button(d, button, pressed);
        eis_device_frame(d, now_us());
    }
    return true;
}

bool Eis::axis(const wlr_pointer_axis_event& e) {
    if (!active_)
        return false;
    eis_device* d = active_->pointer ? active_->pointer : active_->absolute;
    if (!d)
        return true;
    const bool horizontal = e.orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
    if (e.delta == 0)
        eis_device_scroll_stop(d, horizontal, !horizontal);
    else if (e.delta_discrete)
        eis_device_scroll_discrete(d, horizontal ? e.delta_discrete : 0, horizontal ? 0 : e.delta_discrete);
    else
        eis_device_scroll_delta(d, horizontal ? e.delta : 0, horizontal ? 0 : e.delta);
    eis_device_frame(d, now_us());
    return true;
}

bool Eis::key(uint32_t keycode, bool pressed, uint32_t mods, uint32_t sym) {
    if (!active_)
        return false;
    // Super+Shift+Escape takes the keyboard and pointer back (KWin's
    // "Disable Active Input Capture"), and turns the capture off.
    if (pressed && sym == XKB_KEY_Escape && (mods & WLR_MODIFIER_LOGO) && (mods & WLR_MODIFIER_SHIFT)) {
        Session* s = active_;
        deactivate(std::nullopt);
        s->enabled = false;
        s->barriers.clear();
        if (server_.ipc)
            server_.ipc->broadcast("portal", {{"event", "capture.disabled"}, {"cookie", s->cookie}});
        return true;
    }
    if (active_->keyboard) {
        eis_device_keyboard_key(active_->keyboard, keycode, pressed);
        eis_device_frame(active_->keyboard, now_us());
        const wlr_keyboard* kb = server_.seat->physical_keyboard();
        eis_device_keyboard_send_xkb_modifiers(active_->keyboard, kb->modifiers.depressed, kb->modifiers.latched,
                                               kb->modifiers.locked, kb->modifiers.group);
    }
    return true;
}

} // namespace atrium
