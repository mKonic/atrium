#include "wl/ime.hpp"

#include "wl/output.hpp"

#include "input-method-unstable-v2-server.hpp"
#include "text-input-unstable-v3-server.hpp"
#include "virtual-keyboard-unstable-v1-server.hpp"
#include "wlr-virtual-pointer-unstable-v1-server.hpp"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

namespace {

// zwp_input_popup_surface_v2: an input method's candidates, shown while
// they have content.
class PopupRole : public Role {
public:
    static constexpr const char* kName = "zwp_input_popup_surface_v2";
    const char* name() const override { return kName; }
    void commit(Surface& s) override {
        if (s.current().buffer_width > 0)
            s.map();
        else
            s.unmap();
    }
};

} // namespace

namespace {

template <class List>
void detach_all(List& list) {
    for (auto& w : list)
        if (w)
            w->detach();
}

// A keymap as clients get it: a sealed memfd.
int keymap_fd(const std::string& keymap, uint32_t* size) {
    *size = uint32_t(keymap.size() + 1);
    int fd = memfd_create("atrium-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    if (write(fd, keymap.c_str(), *size) != ssize_t(*size)) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    return fd;
}

} // namespace

// ---- text input ---------------------------------------------------------------------

TextInputs::TextInputs(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpTextInputManagerV3>(display, 1, [this](wl_client* client, uint32_t version,
                                                                       uint32_t id) {
        auto* m = make<ZwpTextInputManagerV3>(client, version, id);
        if (!m)
            return;
        m->on_get_text_input([this](ZwpTextInputManagerV3* self, uint32_t id, wl_resource* seat_res) {
            auto* r = make<ZwpTextInputV3>(self->client(), self->version(), id);
            if (!r)
                return;
            if (Seat::from(seat_res) != &seat_) {
                r->detach();
                return;
            }
            auto owned = std::make_unique<TextInput>(TextInput{&seat_, self->client()});
            TextInput* t = owned.get();
            t->resource = r;
            inputs_.push_back(std::move(owned));
            r->on_enable([t](ZwpTextInputV3*) {
                // Enabling resets the state to its defaults first.
                t->pending = State{};
                t->pending.enabled = true;
            });
            r->on_disable([t](ZwpTextInputV3*) { t->pending.enabled = false; });
            r->on_set_surrounding_text([t](ZwpTextInputV3*, const char* text, int32_t cursor, int32_t anchor) {
                t->pending.surrounding = text;
                t->pending.cursor = uint32_t(std::max(cursor, 0));
                t->pending.anchor = uint32_t(std::max(anchor, 0));
            });
            r->on_set_text_change_cause([t](ZwpTextInputV3*, uint32_t cause) { t->pending.change_cause = cause; });
            r->on_set_content_type([t](ZwpTextInputV3*, uint32_t hint, uint32_t purpose) {
                t->pending.content_hint = hint;
                t->pending.content_purpose = purpose;
            });
            r->on_set_cursor_rectangle([t](ZwpTextInputV3*, int32_t x, int32_t y, int32_t w, int32_t h) {
                t->pending.cursor_rect = Box{x, y, w, h};
            });
            r->on_commit([this, t](ZwpTextInputV3*) {
                const bool was = t->current.enabled;
                t->current = t->pending;
                t->pending.surrounding.reset();  // one-shot: must be sent again with changes
                t->pending.change_cause = 0;
                ++t->commits;
                if (!t->focus)
                    return;  // not entered: nothing to act on
                if (t->current.enabled && !was)
                    events.enable.emit(t);
                else if (!t->current.enabled && was)
                    events.disable.emit(t);
                else if (t->current.enabled)
                    events.commit.emit(t);
            });
            r->on_gone([this, t] { drop(t); });
            // Already focused: it enters at once.
            if (Surface* f = seat_.keyboard_focus(); f && f->client() == self->client()) {
                t->focus = f;
                t->focus_gone = f->events.destroy.connect([t] { t->focus = nullptr; });
                r->send_enter(f->resource());
            }
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

TextInputs::~TextInputs() {
    global_.reset();
    detach_all(managers_);
    for (auto& t : inputs_)
        if (Resource* r = t->resource.get())
            r->detach();
}

void TextInputs::drop(TextInput* t) {
    auto it = std::ranges::find_if(inputs_, [t](const auto& x) { return x.get() == t; });
    if (it == inputs_.end())
        return;
    events.destroy.emit(t);
    inputs_.erase(it);
}

void TextInputs::focus(Surface* surface) {
    for (auto& t : inputs_) {
        auto* r = static_cast<ZwpTextInputV3*>(t->resource.get());
        if (!r || r->inert())
            continue;
        if (t->focus && t->focus != surface) {
            if (t->current.enabled)
                events.disable.emit(t.get());
            // A surface going away can't be named; the client knows it went.
            if (wl_resource* gone = t->focus->resource())
                r->send_leave(gone);
            t->focus = nullptr;
            t->focus_gone.disconnect();
        }
        if (surface && !t->focus && t->client == surface->client()) {
            t->focus = surface;
            TextInput* tp = t.get();
            t->focus_gone = surface->events.destroy.connect([tp] { tp->focus = nullptr; });
            r->send_enter(surface->resource());
        }
    }
}

void TextInputs::send_preedit(TextInput* t, const char* text, int32_t begin, int32_t end) {
    if (auto* r = static_cast<ZwpTextInputV3*>(t->resource.get()); r && t->focus)
        r->send_preedit_string(text, begin, end);
}

void TextInputs::send_commit(TextInput* t, const char* text) {
    if (auto* r = static_cast<ZwpTextInputV3*>(t->resource.get()); r && t->focus)
        r->send_commit_string(text);
}

void TextInputs::send_delete(TextInput* t, uint32_t before, uint32_t after) {
    if (auto* r = static_cast<ZwpTextInputV3*>(t->resource.get()); r && t->focus)
        r->send_delete_surrounding_text(before, after);
}

void TextInputs::send_done(TextInput* t) {
    if (auto* r = static_cast<ZwpTextInputV3*>(t->resource.get()); r && t->focus)
        r->send_done(t->commits);
}

// ---- input method ------------------------------------------------------------------------

InputMethods::InputMethods(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpInputMethodManagerV2>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<ZwpInputMethodManagerV2>(client, version, id);
        if (!m)
            return;
        m->on_get_input_method([this](ZwpInputMethodManagerV2* self, wl_resource* seat_res, uint32_t id) {
            auto* r = make<ZwpInputMethodV2>(self->client(), self->version(), id);
            if (!r)
                return;
            // One input method per seat: the next one is told there's no room.
            if (Seat::from(seat_res) != &seat_ || method_) {
                r->send_unavailable();
                r->detach();
                return;
            }
            method_ = std::make_unique<InputMethod>();
            InputMethod* im = method_.get();
            im->seat = &seat_;
            im->resource = r;
            r->on_commit_string([im](ZwpInputMethodV2*, const char* text) { im->pending.commit = text; });
            r->on_set_preedit_string([im](ZwpInputMethodV2*, const char* text, int32_t begin, int32_t end) {
                im->pending.preedit = text;
                im->pending.preedit_begin = begin;
                im->pending.preedit_end = end;
            });
            r->on_delete_surrounding_text([im](ZwpInputMethodV2*, uint32_t before, uint32_t after) {
                im->pending.delete_before = before;
                im->pending.delete_after = after;
            });
            r->on_commit([this, im](ZwpInputMethodV2*, uint32_t serial) {
                // Answering an older state than the latest: dropped, as the
                // protocol says (the text may have moved under it).
                const bool current = serial == im->serial;
                im->current = std::exchange(im->pending, {});
                if (current)
                    events.commit.emit(im);
            });
            r->on_get_input_popup_surface([this, im](ZwpInputMethodV2* self, uint32_t id, wl_resource* surface_res) {
                auto* pr = make<ZwpInputPopupSurfaceV2>(self->client(), self->version(), id);
                Surface* s = Surface::from(surface_res);
                if (!pr)
                    return;
                if (!s) {
                    pr->detach();
                    return;
                }
                auto p = std::make_unique<Popup>(Popup{s});
                Popup* pp = p.get();
                pp->resource = pr;
                // Its role maps it when it has a buffer (as wlroots does):
                // without one it would never show.
                pp->role = std::make_unique<PopupRole>();
                if (!s->set_role(pp->role.get(), self, uint32_t(ZwpInputMethodV2::Error::Role))) {
                    pr->detach();
                    return;
                }
                im->popups.push_back(std::move(p));
                auto drop = [this, im, pp](bool surface_alive) {
                    auto it = std::ranges::find_if(im->popups, [pp](const auto& x) { return x.get() == pp; });
                    if (it == im->popups.end())
                        return;
                    events.destroy_popup.emit(pp);
                    if (Resource* res = pp->resource.get())
                        res->detach();
                    if (surface_alive) {
                        pp->surface->unmap();
                        pp->surface->clear_role(pp->role.get());
                    }
                    im->popups.erase(it);
                };
                pr->on_gone([drop] { drop(true); });
                pp->surface_gone = s->events.destroy.connect([drop] { drop(false); });
                events.new_popup.emit(pp);
                pp->role->commit(*s);  // content already there
            });
            r->on_grab_keyboard([this, im](ZwpInputMethodV2* self, uint32_t id) {
                auto* g = make<ZwpInputMethodKeyboardGrabV2>(self->client(), self->version(), id);
                if (!g)
                    return;
                if (im->grab) {
                    g->detach();  // one grab at a time
                    return;
                }
                im->grab = g;
                if (!keymap_.empty()) {
                    uint32_t size;
                    int fd = keymap_fd(keymap_, &size);
                    if (fd >= 0) {
                        g->send_keymap(1, fd, size);
                        close(fd);
                    }
                }
                g->on_gone([this, im] {
                    im->grab = {};
                    events.grab.emit(false);
                });
                events.grab.emit(true);
            });
            r->on_gone([this] { drop_method(); });
            events.new_method.emit(im);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

InputMethods::~InputMethods() {
    global_.reset();
    detach_all(managers_);
    if (method_) {
        for (auto& p : method_->popups)
            if (Resource* r = p->resource.get())
                r->detach();
        if (Resource* g = method_->grab.get())
            g->detach();
        if (Resource* r = method_->resource.get())
            r->detach();
    }
}

void InputMethods::drop_method() {
    if (!method_)
        return;
    events.destroy.emit(method_.get());
    for (auto& p : method_->popups)
        if (Resource* r = p->resource.get())
            r->detach();
    if (Resource* g = method_->grab.get())
        g->detach();
    method_.reset();
}

void InputMethods::send_state(const TextInputs::State& s) {
    auto* r = method_ ? static_cast<ZwpInputMethodV2*>(method_->resource.get()) : nullptr;
    if (!r)
        return;
    if (s.surrounding)
        r->send_surrounding_text(s.surrounding->c_str(), s.cursor, s.anchor);
    r->send_text_change_cause(s.change_cause);
    r->send_content_type(s.content_hint, s.content_purpose);
}

void InputMethods::activate(const TextInputs::State& s) {
    auto* r = method_ ? static_cast<ZwpInputMethodV2*>(method_->resource.get()) : nullptr;
    if (!r)
        return;
    method_->active = true;
    r->send_activate();
    send_state(s);
    send_done();
}

void InputMethods::deactivate() {
    auto* r = method_ ? static_cast<ZwpInputMethodV2*>(method_->resource.get()) : nullptr;
    if (!r || !method_->active)
        return;
    method_->active = false;
    r->send_deactivate();
    send_done();
}

void InputMethods::send_done() {
    if (auto* r = method_ ? static_cast<ZwpInputMethodV2*>(method_->resource.get()) : nullptr) {
        r->send_done();
        ++method_->serial;
    }
}

bool InputMethods::grabbed() const {
    return method_ && method_->grab;
}

void InputMethods::grab_key(uint32_t time_ms, uint32_t key, bool pressed) {
    if (auto* g = method_ ? static_cast<ZwpInputMethodKeyboardGrabV2*>(method_->grab.get()) : nullptr)
        g->send_key(seat_.next_serial(), time_ms, key, pressed ? 1 : 0);
}

void InputMethods::grab_modifiers(const Seat::Modifiers& m) {
    if (auto* g = method_ ? static_cast<ZwpInputMethodKeyboardGrabV2*>(method_->grab.get()) : nullptr)
        g->send_modifiers(seat_.next_serial(), m.depressed, m.latched, m.locked, m.group);
}

void InputMethods::grab_keymap(const std::string& keymap) {
    keymap_ = keymap;
    if (auto* g = method_ ? static_cast<ZwpInputMethodKeyboardGrabV2*>(method_->grab.get()) : nullptr) {
        uint32_t size;
        int fd = keymap_fd(keymap_, &size);
        if (fd >= 0) {
            g->send_keymap(1, fd, size);
            close(fd);
        }
    }
}

void InputMethods::set_popup_rectangle(Popup* p, const Box& b) {
    if (auto* r = static_cast<ZwpInputPopupSurfaceV2*>(p->resource.get()))
        r->send_text_input_rectangle(b.x, b.y, b.width, b.height);
}

// ---- virtual input ----------------------------------------------------------------------

VirtualInputs::VirtualInputs(wl_display* display, Seat& seat) : seat_(seat) {
    keyboard_global_ = Global::create<ZwpVirtualKeyboardManagerV1>(display, 1, [this](wl_client* client,
                                                                                      uint32_t version, uint32_t id) {
        auto* m = make<ZwpVirtualKeyboardManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_virtual_keyboard([this](ZwpVirtualKeyboardManagerV1* self, wl_resource* seat_res, uint32_t id) {
            auto* r = make<ZwpVirtualKeyboardV1>(self->client(), self->version(), id);
            if (!r)
                return;
            if (Seat::from(seat_res) != &seat_) {
                r->detach();
                return;
            }
            auto owned = std::make_unique<Keyboard>();
            Keyboard* k = owned.get();
            k->resource = r;
            keyboards_.push_back(std::move(owned));
            r->on_keymap([k](ZwpVirtualKeyboardV1* self, uint32_t format, int fd, uint32_t size) {
                if (format != 1 || size == 0) {
                    close(fd);
                    return;
                }
                void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (p == MAP_FAILED) {
                    self->post_no_memory();
                    return;
                }
                k->keymap.assign(static_cast<const char*>(p), strnlen(static_cast<const char*>(p), size));
                munmap(p, size);
                k->keymap_changed.emit();
            });
            r->on_key([k](ZwpVirtualKeyboardV1* self, uint32_t time, uint32_t key, uint32_t state) {
                if (k->keymap.empty()) {
                    self->post_error(uint32_t(ZwpVirtualKeyboardV1::Error::NoKeymap), "no keymap yet");
                    return;
                }
                k->key.emit(time, key, state == 1);
            });
            r->on_modifiers([k](ZwpVirtualKeyboardV1* self, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
                if (k->keymap.empty()) {
                    self->post_error(uint32_t(ZwpVirtualKeyboardV1::Error::NoKeymap), "no keymap yet");
                    return;
                }
                k->modifiers.emit({dep, lat, lock, group});
            });
            r->on_gone([this, k] {
                k->destroy.emit();
                std::erase_if(keyboards_, [k](const auto& x) { return x.get() == k; });
            });
            new_keyboard.emit(k);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    pointer_global_ = Global::create<ZwlrVirtualPointerManagerV1>(display, 2, [this](wl_client* client,
                                                                                     uint32_t version, uint32_t id) {
        auto* m = make<ZwlrVirtualPointerManagerV1>(client, version, id);
        if (!m)
            return;
        auto create = [this](ZwlrVirtualPointerManagerV1* self, wl_resource* seat_res, wl_resource* output_res,
                             uint32_t id) {
            auto* r = make<ZwlrVirtualPointerV1>(self->client(), self->version(), id);
            if (!r)
                return;
            // A null seat means the compositor's.
            if (seat_res && Seat::from(seat_res) != &seat_) {
                r->detach();
                return;
            }
            auto owned = std::make_unique<Pointer>();
            Pointer* p = owned.get();
            p->output = output_res ? Output::from(output_res) : nullptr;
            p->resource = r;
            pointers_.push_back(std::move(owned));
            r->on_motion([p](ZwlrVirtualPointerV1*, uint32_t time, double dx, double dy) {
                p->motion.emit(time, dx, dy);
            });
            r->on_motion_absolute([p](ZwlrVirtualPointerV1*, uint32_t time, uint32_t x, uint32_t y, uint32_t xe,
                                      uint32_t ye) {
                if (xe && ye)
                    p->motion_absolute.emit(time, double(x) / xe, double(y) / ye);
            });
            r->on_button([p](ZwlrVirtualPointerV1*, uint32_t time, uint32_t button, uint32_t state) {
                p->button.emit(time, button, state == 1);
            });
            r->on_axis([p](ZwlrVirtualPointerV1* self, uint32_t time, uint32_t axis, double value) {
                if (axis > 1) {
                    self->post_error(uint32_t(ZwlrVirtualPointerV1::Error::InvalidAxis), "no such axis");
                    return;
                }
                p->axis.emit(time, axis, value, 0);
            });
            r->on_axis_discrete([p](ZwlrVirtualPointerV1* self, uint32_t time, uint32_t axis, double value,
                                    int32_t discrete) {
                if (axis > 1) {
                    self->post_error(uint32_t(ZwlrVirtualPointerV1::Error::InvalidAxis), "no such axis");
                    return;
                }
                p->axis.emit(time, axis, value, discrete);
            });
            r->on_axis_source([p](ZwlrVirtualPointerV1* self, uint32_t source) {
                if (source > 3) {
                    self->post_error(uint32_t(ZwlrVirtualPointerV1::Error::InvalidAxisSource), "no such source");
                    return;
                }
                p->axis_source.emit(source);
            });
            r->on_axis_stop([p](ZwlrVirtualPointerV1*, uint32_t time, uint32_t axis) { p->axis_stop.emit(time, axis); });
            r->on_frame([p](ZwlrVirtualPointerV1*) { p->frame.emit(); });
            r->on_gone([this, p] {
                p->destroy.emit();
                std::erase_if(pointers_, [p](const auto& x) { return x.get() == p; });
            });
            new_pointer.emit(p);
        };
        m->on_create_virtual_pointer([create](ZwlrVirtualPointerManagerV1* self, wl_resource* seat, uint32_t id) {
            create(self, seat, nullptr, id);
        });
        m->on_create_virtual_pointer_with_output([create](ZwlrVirtualPointerManagerV1* self, wl_resource* seat,
                                                          wl_resource* output, uint32_t id) {
            create(self, seat, output, id);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

VirtualInputs::~VirtualInputs() {
    keyboard_global_.reset();
    pointer_global_.reset();
    detach_all(managers_);
    for (auto& k : keyboards_)
        if (Resource* r = k->resource.get())
            r->detach();
    for (auto& p : pointers_)
        if (Resource* r = p->resource.get())
            r->detach();
}

} // namespace atrium::wl
