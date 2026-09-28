#include "wl/seat.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

namespace {

// The pointer image's role: a surface a client set with wl_pointer.set_cursor.
class CursorRole : public Role {
public:
    static constexpr const char* kName = "wl_pointer-cursor";
    const char* name() const override { return kName; }
};
CursorRole g_cursor_role;

wl_array keys_array(const std::vector<uint32_t>& keys) {
    wl_array a;
    wl_array_init(&a);
    auto* p = static_cast<uint32_t*>(wl_array_add(&a, keys.size() * sizeof(uint32_t)));
    if (p)
        std::copy(keys.begin(), keys.end(), p);
    return a;
}

} // namespace

SeatResource::SeatResource(wl_client* client, uint32_t version, uint32_t id, Seat* s)
    : WlSeat(client, version, id), seat(s) {
    on_get_pointer([this](WlSeat*, uint32_t id) {
        auto* p = make<WlPointer>(this->client(), this->version(), id);
        if (!p || !seat) {
            if (p)
                p->detach();
            return;
        }
        p->on_set_cursor([this](WlPointer* self, uint32_t serial, WlSurface* surface_resource, int32_t hx,
                                int32_t hy) {
            if (!seat || !seat->pointer_focus_ || seat->pointer_focus_->client() != self->client() ||
                serial != seat->pointer_enter_serial_)
                return;  // stale: another surface has the pointer now
            auto* surface = dynamic_cast<Surface*>(surface_resource);
            if (surface) {
                if (surface->role_name() && surface->role_name() != CursorRole::kName) {
                    self->post_error(uint32_t(WlPointer::Error::Role), "the surface has another role");
                    return;
                }
                if (!surface->role())
                    surface->set_role(&g_cursor_role, nullptr, 0);
            }
            seat->events.request_cursor.emit({self->client(), surface, hx, hy});
        });
        std::erase_if(pointers, [](const auto& w) { return !w; });
        pointers.push_back(p);
        // Already over one of this client's surfaces: this pointer hears so.
        if (Surface* f = seat->pointer_focus_; f && f->client() == this->client()) {
            p->send_enter(seat->pointer_enter_serial_, f, seat->pointer_x_, seat->pointer_y_);
            if (p->version() >= 5)
                p->send_frame();
        }
    });
    on_get_keyboard([this](WlSeat*, uint32_t id) {
        auto* k = make<WlKeyboard>(this->client(), this->version(), id);
        if (!k || !seat) {
            if (k)
                k->detach();
            return;
        }
        std::erase_if(keyboards, [](const auto& w) { return !w; });
        keyboards.push_back(k);
        seat->bind_keyboard(k);
    });
    on_get_touch([this](WlSeat*, uint32_t id) {
        auto* t = make<WlTouch>(this->client(), this->version(), id);
        if (!t || !seat) {
            if (t)
                t->detach();
            return;
        }
        std::erase_if(touches, [](const auto& w) { return !w; });
        touches.push_back(t);
    });
}

Seat::Seat(wl_display* display, std::string name) : display_(display), name_(std::move(name)) {
    global_ = Global::create<WlSeat>(display, 9, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* r = make<SeatResource>(client, version, id, this);
        if (!r)
            return;
        std::erase_if(resources_, [](const auto& w) { return !w; });
        resources_.push_back(r);
        r->send_capabilities(caps_);
        if (r->version() >= 2)
            r->send_name(name_.c_str());
    });
}

Seat::~Seat() {
    global_.reset();
    for (auto& w : resources_)
        if (SeatResource* r = w.get()) {
            r->seat = nullptr;
            for (auto& p : r->pointers)
                if (p)
                    p->detach();
            for (auto& k : r->keyboards)
                if (k)
                    k->detach();
            for (auto& t : r->touches)
                if (t)
                    t->detach();
            r->detach();
        }
}

Seat* Seat::from(WlSeat* resource) {
    auto* r = dynamic_cast<SeatResource*>(resource);
    return r ? r->seat : nullptr;
}

Seat* Seat::from(wl_resource* resource) {
    return from(WlSeat::from(resource));
}

void Seat::set_capabilities(uint32_t caps) {
    if (caps == caps_)
        return;
    caps_ = caps;
    for (auto& w : resources_)
        if (SeatResource* r = w.get())
            r->send_capabilities(caps);
}

uint32_t Seat::next_serial() {
    return wl_display_next_serial(display_);
}

void Seat::remember_serial(wl_client* client, uint32_t serial) {
    serials_[next_serial_slot_] = {client, serial};
    next_serial_slot_ = (next_serial_slot_ + 1) % serials_.size();
}

bool Seat::validate_grab_serial(wl_client* client, uint32_t serial) const {
    return std::ranges::any_of(serials_, [&](const Serial& s) { return s.client == client && s.serial == serial; });
}

std::vector<SeatResource*> Seat::resources_for(wl_client* client) const {
    std::vector<SeatResource*> out;
    for (const auto& w : resources_)
        if (SeatResource* r = w.get(); r && r->client() == client)
            out.push_back(r);
    return out;
}

std::vector<WlKeyboard*> Seat::keyboards_for(wl_client* client) const {
    std::vector<WlKeyboard*> out;
    for (SeatResource* r : resources_for(client))
        for (const auto& k : r->keyboards)
            if (k)
                out.push_back(k.get());
    return out;
}

template <class Fn>
void Seat::each_pointer(wl_client* client, Fn fn) {
    for (SeatResource* r : resources_for(client))
        for (const auto& p : std::vector(r->pointers))
            if (WlPointer* x = p.get())
                fn(x);
}

template <class Fn>
void Seat::each_keyboard(wl_client* client, Fn fn) {
    for (SeatResource* r : resources_for(client))
        for (const auto& k : std::vector(r->keyboards))
            if (WlKeyboard* x = k.get())
                fn(x);
}

template <class Fn>
void Seat::each_touch(wl_client* client, Fn fn) {
    for (SeatResource* r : resources_for(client))
        for (const auto& t : std::vector(r->touches))
            if (WlTouch* x = t.get())
                fn(x);
}

// ---- keyboard ---------------------------------------------------------------------

void Seat::set_keymap(const std::string& keymap) {
    keymap_ = keymap;
    for (auto& w : resources_)
        if (SeatResource* r = w.get())
            for (auto& k : r->keyboards)
                if (k)
                    send_keymap(k.get());
}

void Seat::send_keymap(WlKeyboard* k) {
    if (keymap_.empty()) {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        k->send_keymap(uint32_t(WlKeyboard::KeymapFormat::NoKeymap), fd, 0);
        close(fd);
        return;
    }
    // A sealed copy per keyboard: clients map it, none can change it.
    const size_t size = keymap_.size() + 1;
    int fd = memfd_create("atrium-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return;
    if (write(fd, keymap_.c_str(), size) != ssize_t(size)) {
        close(fd);
        return;
    }
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    k->send_keymap(uint32_t(WlKeyboard::KeymapFormat::XkbV1), fd, uint32_t(size));
    close(fd);
}

void Seat::set_repeat_info(int32_t rate, int32_t delay) {
    repeat_rate_ = rate;
    repeat_delay_ = delay;
    for (auto& w : resources_)
        if (SeatResource* r = w.get())
            for (auto& k : r->keyboards)
                if (k && k->version() >= 4)
                    k->send_repeat_info(rate, delay);
}

void Seat::bind_keyboard(WlKeyboard* k) {
    send_keymap(k);
    if (k->version() >= 4)
        k->send_repeat_info(repeat_rate_, repeat_delay_);
    if (keyboard_focus_ && keyboard_focus_->client() == k->client()) {
        wl_array keys = keys_array(keys_);
        const uint32_t serial = next_serial();
        k->send_enter(serial, keyboard_focus_, &keys);
        wl_array_release(&keys);
        k->send_modifiers(serial, mods_.depressed, mods_.latched, mods_.locked, mods_.group);
    }
}

void Seat::keyboard_enter(Surface* surface, const std::vector<uint32_t>& pressed, const Modifiers& mods) {
    if (surface == keyboard_focus_) {
        keyboard_modifiers(mods);
        return;
    }
    wl_client* old_client = keyboard_focus_ ? keyboard_focus_->client() : nullptr;
    if (keyboard_focus_) {
        const uint32_t serial = next_serial();
        Surface* old = keyboard_focus_;
        each_keyboard(old->client(), [&](WlKeyboard* k) { k->send_leave(serial, old); });
    }
    keyboard_focus_ = surface;
    keyboard_focus_gone_.disconnect();
    keys_ = pressed;
    mods_ = mods;
    if (surface) {
        keyboard_focus_gone_ = surface->events.destroy.connect([this] {
            wl_client* was = keyboard_focus_ ? keyboard_focus_->client() : nullptr;
            keyboard_focus_ = nullptr;
            keyboard_focus_gone_.disconnect();
            events.keyboard_focus.emit(nullptr);
            if (was)
                events.keyboard_client.emit(nullptr);
        });
        wl_array keys = keys_array(keys_);
        const uint32_t serial = next_serial();
        each_keyboard(surface->client(), [&](WlKeyboard* k) {
            k->send_enter(serial, surface, &keys);
            k->send_modifiers(serial, mods.depressed, mods.latched, mods.locked, mods.group);
        });
        wl_array_release(&keys);
    }
    events.keyboard_focus.emit(surface);
    wl_client* new_client = surface ? surface->client() : nullptr;
    if (new_client != old_client)
        events.keyboard_client.emit(new_client);
}

void Seat::keyboard_clear_focus() {
    keyboard_enter(nullptr, {}, mods_);
}

void Seat::keyboard_key(uint32_t time_ms, uint32_t key, bool pressed) {
    if (pressed) {
        if (std::ranges::find(keys_, key) == keys_.end())
            keys_.push_back(key);
    } else {
        std::erase(keys_, key);
    }
    if (!keyboard_focus_)
        return;
    const uint32_t serial = next_serial();
    // A key press is input the client really got: it may start an
    // activation with it.
    if (pressed)
        remember_serial(keyboard_focus_->client(), serial);
    each_keyboard(keyboard_focus_->client(), [&](WlKeyboard* k) {
        k->send_key(serial, time_ms, key,
                    uint32_t(pressed ? WlKeyboard::KeyState::Pressed : WlKeyboard::KeyState::Released));
    });
}

void Seat::keyboard_modifiers(const Modifiers& mods) {
    if (mods == mods_)
        return;
    mods_ = mods;
    if (!keyboard_focus_)
        return;
    const uint32_t serial = next_serial();
    each_keyboard(keyboard_focus_->client(), [&](WlKeyboard* k) {
        k->send_modifiers(serial, mods.depressed, mods.latched, mods.locked, mods.group);
    });
}

// ---- pointer ------------------------------------------------------------------------

void Seat::pointer_enter(Surface* surface, double sx, double sy) {
    if (surface == pointer_focus_) {
        pointer_x_ = sx;
        pointer_y_ = sy;
        return;
    }
    if (pointer_focus_) {
        const uint32_t serial = next_serial();
        Surface* old = pointer_focus_;
        each_pointer(old->client(), [&](WlPointer* p) {
            p->send_leave(serial, old);
            if (p->version() >= 5)
                p->send_frame();
        });
    }
    pointer_focus_ = surface;
    pointer_focus_gone_.disconnect();
    buttons_.clear();
    pointer_x_ = sx;
    pointer_y_ = sy;
    if (surface) {
        pointer_focus_gone_ = surface->events.destroy.connect([this] {
            pointer_focus_ = nullptr;
            pointer_focus_gone_.disconnect();
            events.pointer_focus.emit(nullptr);
        });
        pointer_enter_serial_ = next_serial();
        each_pointer(surface->client(), [&](WlPointer* p) {
            p->send_enter(pointer_enter_serial_, surface, sx, sy);
            if (p->version() >= 5)
                p->send_frame();
        });
    }
    events.pointer_focus.emit(surface);
}

void Seat::pointer_clear_focus() {
    pointer_enter(nullptr, 0, 0);
}

void Seat::pointer_motion(uint32_t time_ms, double sx, double sy) {
    pointer_x_ = sx;
    pointer_y_ = sy;
    if (!pointer_focus_)
        return;
    each_pointer(pointer_focus_->client(), [&](WlPointer* p) { p->send_motion(time_ms, sx, sy); });
    pointer_frame_pending_ = true;
}

uint32_t Seat::pointer_button(uint32_t time_ms, uint32_t button, bool pressed) {
    if (pressed) {
        if (std::ranges::find(buttons_, button) == buttons_.end())
            buttons_.push_back(button);
    } else {
        std::erase(buttons_, button);
    }
    if (!pointer_focus_)
        return 0;
    const uint32_t serial = next_serial();
    if (pressed)
        remember_serial(pointer_focus_->client(), serial);
    each_pointer(pointer_focus_->client(), [&](WlPointer* p) {
        p->send_button(serial, time_ms, button,
                       uint32_t(pressed ? WlPointer::ButtonState::Pressed : WlPointer::ButtonState::Released));
    });
    pointer_frame_pending_ = true;
    return serial;
}

void Seat::pointer_axis(uint32_t time_ms, uint32_t orientation, double value, int32_t value120, AxisSource source,
                        bool inverted) {
    if (!pointer_focus_)
        return;
    each_pointer(pointer_focus_->client(), [&](WlPointer* p) {
        const uint32_t v = p->version();
        if (v >= 5)
            p->send_axis_source(uint32_t(source));
        if (value120 != 0) {
            if (v >= 8)
                p->send_axis_value120(orientation, value120);
            else if (v >= 5)
                p->send_axis_discrete(orientation, value120 / 120);
        }
        if (v >= 9)
            p->send_axis_relative_direction(orientation, inverted ? 1 : 0);
        if (value != 0)
            p->send_axis(time_ms, orientation, value);
        else if (v >= 5)
            p->send_axis_stop(time_ms, orientation);
    });
    pointer_frame_pending_ = true;
}

void Seat::pointer_frame() {
    if (!pointer_focus_ || !std::exchange(pointer_frame_pending_, false))
        return;
    each_pointer(pointer_focus_->client(), [&](WlPointer* p) {
        if (p->version() >= 5)
            p->send_frame();
    });
}

// ---- touch ----------------------------------------------------------------------------

uint32_t Seat::touch_down(uint32_t time_ms, Surface* surface, int32_t id, double sx, double sy) {
    if (!surface)
        return 0;
    auto point = std::make_unique<TouchPoint>(TouchPoint{id, surface, {}});
    TouchPoint* tp = point.get();
    tp->gone = surface->events.destroy.connect([tp] { tp->surface = nullptr; });
    std::erase_if(touches_, [id](const auto& t) { return t->id == id; });
    touches_.push_back(std::move(point));
    const uint32_t serial = next_serial();
    remember_serial(surface->client(), serial);
    each_touch(surface->client(), [&](WlTouch* t) { t->send_down(serial, time_ms, surface, id, sx, sy); });
    return serial;
}

void Seat::touch_up(uint32_t time_ms, int32_t id) {
    auto it = std::ranges::find_if(touches_, [id](const auto& t) { return t->id == id; });
    if (it == touches_.end())
        return;
    if (Surface* s = (*it)->surface) {
        const uint32_t serial = next_serial();
        each_touch(s->client(), [&](WlTouch* t) { t->send_up(serial, time_ms, id); });
    }
    // Kept until the frame, which goes to its client too.
    (*it)->id = -1 - (*it)->id;
}

void Seat::touch_motion(uint32_t time_ms, int32_t id, double sx, double sy) {
    for (const auto& tp : touches_)
        if (tp->id == id && tp->surface)
            each_touch(tp->surface->client(), [&](WlTouch* t) { t->send_motion(time_ms, id, sx, sy); });
}

void Seat::touch_frame() {
    std::vector<wl_client*> clients;
    for (const auto& tp : touches_)
        if (tp->surface && std::ranges::find(clients, tp->surface->client()) == clients.end())
            clients.push_back(tp->surface->client());
    for (wl_client* c : clients)
        each_touch(c, [](WlTouch* t) { t->send_frame(); });
    std::erase_if(touches_, [](const auto& t) { return t->id < 0 || !t->surface; });
}

void Seat::touch_cancel() {
    std::vector<wl_client*> clients;
    for (const auto& tp : touches_)
        if (tp->surface && std::ranges::find(clients, tp->surface->client()) == clients.end())
            clients.push_back(tp->surface->client());
    for (wl_client* c : clients)
        each_touch(c, [](WlTouch* t) { t->send_cancel(); });
    touches_.clear();
}

} // namespace atrium::wl
