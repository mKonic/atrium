#include "wl/input_ext.hpp"

#include "cursor-shape-v1-server.hpp"
#include "ext-idle-notify-v1-server.hpp"
#include "idle-inhibit-unstable-v1-server.hpp"
#include "keyboard-shortcuts-inhibit-unstable-v1-server.hpp"
#include "pointer-constraints-unstable-v1-server.hpp"
#include "pointer-gestures-unstable-v1-server.hpp"
#include "pointer-warp-v1-server.hpp"
#include "relative-pointer-unstable-v1-server.hpp"

#include <algorithm>

namespace atrium::wl {

namespace {

template <class List>
void detach_all(List& list) {
    for (auto& w : list)
        if (w)
            w->detach();
}

template <class Owned>
void detach_owned(Owned& list) {
    for (auto& x : list)
        if (Resource* r = x->resource.get())
            r->detach();
}

// Whether a per-pointer object `r` belongs to the client with pointer focus.
bool focused(Seat& seat, Resource* r) {
    return r && !r->inert() && seat.pointer_focus() && seat.pointer_focus()->client() == r->client();
}

} // namespace

// ---- relative pointer --------------------------------------------------------------

RelativePointers::RelativePointers(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpRelativePointerManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                             uint32_t id) {
        auto* m = make<ZwpRelativePointerManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_relative_pointer([this](ZwpRelativePointerManagerV1* self, uint32_t id, wl_resource* pointer) {
            auto* r = make<ZwpRelativePointerV1>(self->client(), self->version(), id);
            if (!r)
                return;
            if (!seat_.has_pointer(pointer)) {
                r->detach();
                return;
            }
            std::erase_if(pointers_, [](const auto& w) { return !w; });
            pointers_.push_back(r);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

RelativePointers::~RelativePointers() {
    global_.reset();
    detach_all(managers_);
    detach_all(pointers_);
}

void RelativePointers::send_motion(uint64_t time_us, double dx, double dy, double dx_unaccel, double dy_unaccel) {
    bool sent = false;
    for (auto& w : pointers_)
        if (focused(seat_, w.get())) {
            static_cast<ZwpRelativePointerV1*>(w.get())
                ->send_relative_motion(uint32_t(time_us >> 32), uint32_t(time_us & 0xffffffff), dx, dy, dx_unaccel,
                                       dy_unaccel);
            sent = true;
        }
    // A locked pointer sends no motion of its own; without the frame after
    // it, Xwayland never hands a game this motion (mouselook stands still).
    if (sent)
        seat_.pointer_frame_needed();
}

std::vector<wl_client*> RelativePointers::clients() const {
    std::vector<wl_client*> out;
    for (const auto& w : pointers_)
        if (Resource* r = w.get(); r && !r->inert() && std::ranges::find(out, r->client()) == out.end())
            out.push_back(r->client());
    return out;
}

// ---- pointer constraints ----------------------------------------------------------------

PointerConstraints::PointerConstraints(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpPointerConstraintsV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<ZwpPointerConstraintsV1>(client, version, id);
        if (!m)
            return;
        auto create = [this](ZwpPointerConstraintsV1* self, Type type, uint32_t id, wl_resource* surface_res,
                             wl_resource* pointer, wl_resource* region_res, uint32_t lifetime) {
            Resource* r = type == Type::Lock
                              ? static_cast<Resource*>(make<ZwpLockedPointerV1>(self->client(), self->version(), id))
                              : static_cast<Resource*>(make<ZwpConfinedPointerV1>(self->client(), self->version(), id));
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s || !seat_.has_pointer(pointer)) {
                r->detach();
                return;
            }
            if (for_surface(s)) {
                self->post_error(uint32_t(ZwpPointerConstraintsV1::Error::AlreadyConstrained),
                                 "the surface already has a constraint");
                return;
            }
            auto owned = std::make_unique<Constraint>(Constraint{
                type, s, Region::infinite(), lifetime == uint32_t(ZwpPointerConstraintsV1::Lifetime::Persistent)});
            Constraint* c = owned.get();
            c->resource = r;
            if (auto* region = region_res ? dynamic_cast<RegionResource*>(WlRegion::from(region_res)) : nullptr)
                c->region = region->region;
            constraints_.push_back(std::move(owned));
            // The region (and the lock's hint) change with the surface's commit.
            c->commit = s->events.commit.connect([this, c] {
                if (c->region_pending) {
                    c->region = std::move(c->pending_region);
                    c->region_pending = false;
                    events.region_changed.emit(c);
                }
                if (c->pending_hint)
                    c->cursor_hint = std::exchange(c->pending_hint, std::nullopt);
            });
            c->surface_gone = s->events.destroy.connect([this, c] { drop(c); });
            r->on_gone([this, c] { drop(c); });
            auto set_region = [c](wl_resource* region_res) {
                auto* region = region_res ? dynamic_cast<RegionResource*>(WlRegion::from(region_res)) : nullptr;
                c->pending_region = region ? region->region : Region::infinite();
                c->region_pending = true;
            };
            if (type == Type::Lock) {
                auto* l = static_cast<ZwpLockedPointerV1*>(r);
                l->on_set_region([set_region](ZwpLockedPointerV1*, wl_resource* region) { set_region(region); });
                l->on_set_cursor_position_hint([c](ZwpLockedPointerV1*, double x, double y) {
                    c->pending_hint = std::make_pair(x, y);
                });
            } else {
                static_cast<ZwpConfinedPointerV1*>(r)->on_set_region(
                    [set_region](ZwpConfinedPointerV1*, wl_resource* region) { set_region(region); });
            }
            events.new_constraint.emit(c);
        };
        m->on_lock_pointer([create](ZwpPointerConstraintsV1* self, uint32_t id, wl_resource* surface,
                                    wl_resource* pointer, wl_resource* region, uint32_t lifetime) {
            create(self, Type::Lock, id, surface, pointer, region, lifetime);
        });
        m->on_confine_pointer([create](ZwpPointerConstraintsV1* self, uint32_t id, wl_resource* surface,
                                       wl_resource* pointer, wl_resource* region, uint32_t lifetime) {
            create(self, Type::Confine, id, surface, pointer, region, lifetime);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

PointerConstraints::~PointerConstraints() {
    global_.reset();
    detach_all(managers_);
    detach_owned(constraints_);
}

PointerConstraints::Constraint* PointerConstraints::for_surface(Surface* surface) const {
    for (const auto& c : constraints_)
        if (c->surface == surface)
            return c.get();
    return nullptr;
}

void PointerConstraints::activate(Constraint* c) {
    if (c->active || c->spent)
        return;
    c->active = true;
    if (Resource* r = c->resource.get()) {
        if (c->type == Type::Lock)
            static_cast<ZwpLockedPointerV1*>(r)->send_locked();
        else
            static_cast<ZwpConfinedPointerV1*>(r)->send_confined();
    }
}

void PointerConstraints::deactivate(Constraint* c) {
    if (!c->active)
        return;
    c->active = false;
    if (Resource* r = c->resource.get()) {
        if (c->type == Type::Lock)
            static_cast<ZwpLockedPointerV1*>(r)->send_unlocked();
        else
            static_cast<ZwpConfinedPointerV1*>(r)->send_unconfined();
    }
    // A one-shot constraint is spent: the client makes a new one to try again.
    if (!c->persistent)
        c->spent = true;
}

void PointerConstraints::drop(Constraint* c) {
    auto it = std::ranges::find_if(constraints_, [c](const auto& x) { return x.get() == c; });
    if (it == constraints_.end())
        return;
    events.destroy.emit(c);
    if (Resource* r = c->resource.get())
        r->detach();
    constraints_.erase(it);
}

// ---- pointer gestures ------------------------------------------------------------------

PointerGestures::PointerGestures(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpPointerGesturesV1>(display, 3, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<ZwpPointerGesturesV1>(client, version, id);
        if (!m)
            return;
        auto keep = [this](std::vector<Weak<Resource>>& list, Resource* r, wl_resource* pointer) {
            if (!r)
                return;
            if (!seat_.has_pointer(pointer)) {
                r->detach();
                return;
            }
            std::erase_if(list, [](const auto& w) { return !w; });
            list.push_back(r);
        };
        m->on_get_swipe_gesture([this, keep](ZwpPointerGesturesV1* self, uint32_t id, wl_resource* pointer) {
            keep(swipes_, make<ZwpPointerGestureSwipeV1>(self->client(), self->version(), id), pointer);
        });
        m->on_get_pinch_gesture([this, keep](ZwpPointerGesturesV1* self, uint32_t id, wl_resource* pointer) {
            keep(pinches_, make<ZwpPointerGesturePinchV1>(self->client(), self->version(), id), pointer);
        });
        m->on_get_hold_gesture([this, keep](ZwpPointerGesturesV1* self, uint32_t id, wl_resource* pointer) {
            keep(holds_, make<ZwpPointerGestureHoldV1>(self->client(), self->version(), id), pointer);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

PointerGestures::~PointerGestures() {
    global_.reset();
    for (auto* list : {&managers_, &swipes_, &pinches_, &holds_})
        detach_all(*list);
}

// A gesture goes to the client whose surface it began over, to the end.
template <class T, class Fn>
void PointerGestures::each(std::vector<Weak<Resource>>& list, Fn fn) {
    if (!gesture_surface_)
        return;
    for (auto& w : std::vector(list))
        if (Resource* r = w.get(); r && !r->inert() && r->client() == gesture_surface_->client())
            fn(static_cast<T*>(r));
}

namespace {
uint32_t serial_of(Seat& seat) {
    return seat.next_serial();
}
} // namespace

void PointerGestures::swipe_begin(uint32_t time_ms, uint32_t fingers) {
    gesture_surface_ = seat_.pointer_focus();
    gesture_gone_ = gesture_surface_ ? gesture_surface_->events.destroy.connect([this] { gesture_surface_ = nullptr; })
                                     : Connection{};
    const uint32_t serial = serial_of(seat_);
    each<ZwpPointerGestureSwipeV1>(swipes_, [&](auto* g) { g->send_begin(serial, time_ms, gesture_surface_->resource(), fingers); });
}
void PointerGestures::swipe_update(uint32_t time_ms, double dx, double dy) {
    each<ZwpPointerGestureSwipeV1>(swipes_, [&](auto* g) { g->send_update(time_ms, dx, dy); });
}
void PointerGestures::swipe_end(uint32_t time_ms, bool cancelled) {
    const uint32_t serial = serial_of(seat_);
    each<ZwpPointerGestureSwipeV1>(swipes_, [&](auto* g) { g->send_end(serial, time_ms, cancelled ? 1 : 0); });
    gesture_surface_ = nullptr;
}
void PointerGestures::pinch_begin(uint32_t time_ms, uint32_t fingers) {
    gesture_surface_ = seat_.pointer_focus();
    gesture_gone_ = gesture_surface_ ? gesture_surface_->events.destroy.connect([this] { gesture_surface_ = nullptr; })
                                     : Connection{};
    const uint32_t serial = serial_of(seat_);
    each<ZwpPointerGesturePinchV1>(pinches_, [&](auto* g) { g->send_begin(serial, time_ms, gesture_surface_->resource(), fingers); });
}
void PointerGestures::pinch_update(uint32_t time_ms, double dx, double dy, double scale, double rotation) {
    each<ZwpPointerGesturePinchV1>(pinches_, [&](auto* g) { g->send_update(time_ms, dx, dy, scale, rotation); });
}
void PointerGestures::pinch_end(uint32_t time_ms, bool cancelled) {
    const uint32_t serial = serial_of(seat_);
    each<ZwpPointerGesturePinchV1>(pinches_, [&](auto* g) { g->send_end(serial, time_ms, cancelled ? 1 : 0); });
    gesture_surface_ = nullptr;
}
void PointerGestures::hold_begin(uint32_t time_ms, uint32_t fingers) {
    gesture_surface_ = seat_.pointer_focus();
    gesture_gone_ = gesture_surface_ ? gesture_surface_->events.destroy.connect([this] { gesture_surface_ = nullptr; })
                                     : Connection{};
    const uint32_t serial = serial_of(seat_);
    each<ZwpPointerGestureHoldV1>(holds_, [&](auto* g) {
        if (g->version() >= 3)
            g->send_begin(serial, time_ms, gesture_surface_->resource(), fingers);
    });
}
void PointerGestures::hold_end(uint32_t time_ms, bool cancelled) {
    const uint32_t serial = serial_of(seat_);
    each<ZwpPointerGestureHoldV1>(holds_, [&](auto* g) {
        if (g->version() >= 3)
            g->send_end(serial, time_ms, cancelled ? 1 : 0);
    });
    gesture_surface_ = nullptr;
}

// ---- keyboard shortcuts inhibit ------------------------------------------------------------

ShortcutInhibitors::ShortcutInhibitors(wl_display* display) {
    global_ = Global::create<ZwpKeyboardShortcutsInhibitManagerV1>(display, 1, [this](wl_client* client,
                                                                                      uint32_t version, uint32_t id) {
        auto* m = make<ZwpKeyboardShortcutsInhibitManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_inhibit_shortcuts([this](ZwpKeyboardShortcutsInhibitManagerV1* self, uint32_t id,
                                       wl_resource* surface_res, wl_resource* seat_res) {
            auto* r = make<ZwpKeyboardShortcutsInhibitorV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            Seat* seat = Seat::from(seat_res);
            if (!r)
                return;
            if (!s || !seat) {
                r->detach();
                return;
            }
            if (std::ranges::any_of(inhibitors_, [&](const auto& i) { return i->surface == s && i->seat == seat; })) {
                self->post_error(uint32_t(ZwpKeyboardShortcutsInhibitManagerV1::Error::AlreadyInhibited),
                                 "the surface already inhibits shortcuts on this seat");
                return;
            }
            auto owned = std::make_unique<Inhibitor>(Inhibitor{s, seat});
            Inhibitor* i = owned.get();
            i->resource = r;
            inhibitors_.push_back(std::move(owned));
            i->surface_gone = s->events.destroy.connect([this, i] { drop(i); });
            r->on_gone([this, i] { drop(i); });
            new_inhibitor.emit(i);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

ShortcutInhibitors::~ShortcutInhibitors() {
    global_.reset();
    detach_all(managers_);
    detach_owned(inhibitors_);
}

ShortcutInhibitors::Inhibitor* ShortcutInhibitors::for_surface(Surface* surface) const {
    for (const auto& i : inhibitors_)
        if (i->surface == surface)
            return i.get();
    return nullptr;
}

void ShortcutInhibitors::set_active(Inhibitor* i, bool active) {
    if (i->active == active)
        return;
    i->active = active;
    if (auto* r = static_cast<ZwpKeyboardShortcutsInhibitorV1*>(i->resource.get())) {
        if (active)
            r->send_active();
        else
            r->send_inactive();
    }
}

void ShortcutInhibitors::drop(Inhibitor* i) {
    auto it = std::ranges::find_if(inhibitors_, [i](const auto& x) { return x.get() == i; });
    if (it == inhibitors_.end())
        return;
    destroy.emit(i);
    if (Resource* r = i->resource.get())
        r->detach();
    inhibitors_.erase(it);
}

// ---- cursor shape ---------------------------------------------------------------------------

CursorShapes::CursorShapes(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<WpCursorShapeManagerV1>(display, 2, [this](wl_client* client, uint32_t version,
                                                                        uint32_t id) {
        auto* m = make<WpCursorShapeManagerV1>(client, version, id);
        if (!m)
            return;
        auto device = [this](WpCursorShapeManagerV1* self, uint32_t id, bool tablet, bool ours) {
            auto* d = make<WpCursorShapeDeviceV1>(self->client(), self->version(), id);
            if (!d)
                return;
            if (!ours) {
                d->detach();
                return;
            }
            d->on_set_shape([this, tablet](WpCursorShapeDeviceV1* self, uint32_t serial, uint32_t shape) {
                const uint32_t last = self->version() >= 2 ? 36 : 34;
                if (shape < 1 || shape > last) {
                    self->post_error(uint32_t(WpCursorShapeDeviceV1::Error::InvalidShape), "no such shape");
                    return;
                }
                request_shape.emit({self->client(), serial, shape, tablet});
            });
            std::erase_if(devices_, [](const auto& w) { return !w; });
            devices_.push_back(d);
        };
        m->on_get_pointer([this, device](WpCursorShapeManagerV1* self, uint32_t id, wl_resource* pointer) {
            device(self, id, false, seat_.has_pointer(pointer));
        });
        m->on_get_tablet_tool_v2([device](WpCursorShapeManagerV1* self, uint32_t id, wl_resource*) {
            device(self, id, true, true);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

CursorShapes::~CursorShapes() {
    global_.reset();
    detach_all(managers_);
    detach_all(devices_);
}

const char* CursorShapes::name_of(uint32_t shape) {
    static const char* const names[] = {
        nullptr, "default", "context-menu", "help", "pointer", "progress", "wait", "cell", "crosshair", "text",
        "vertical-text", "alias", "copy", "move", "no-drop", "not-allowed", "grab", "grabbing", "e-resize",
        "n-resize", "ne-resize", "nw-resize", "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize",
        "ns-resize", "nesw-resize", "nwse-resize", "col-resize", "row-resize", "all-scroll", "zoom-in", "zoom-out",
        "dnd-ask", "all-resize",
    };
    return shape < std::size(names) && names[shape] ? names[shape] : "default";
}

// ---- idle inhibit ---------------------------------------------------------------------------

IdleInhibitors::IdleInhibitors(wl_display* display) {
    global_ = Global::create<ZwpIdleInhibitManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<ZwpIdleInhibitManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_inhibitor([this](ZwpIdleInhibitManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<ZwpIdleInhibitorV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s) {
                r->detach();
                return;
            }
            auto owned = std::make_unique<Inhibitor>(Inhibitor{s});
            Inhibitor* i = owned.get();
            i->resource = r;
            inhibitors_.push_back(std::move(owned));
            auto drop = [this, i] {
                auto it = std::ranges::find_if(inhibitors_, [i](const auto& x) { return x.get() == i; });
                if (it == inhibitors_.end())
                    return;
                if (Resource* res = i->resource.get())
                    res->detach();
                inhibitors_.erase(it);
                changed.emit();
            };
            i->surface_gone = s->events.destroy.connect(drop);
            r->on_gone(drop);
            changed.emit();
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

IdleInhibitors::~IdleInhibitors() {
    global_.reset();
    detach_all(managers_);
    detach_owned(inhibitors_);
}

std::vector<Surface*> IdleInhibitors::surfaces() const {
    std::vector<Surface*> out;
    for (const auto& i : inhibitors_)
        out.push_back(i->surface);
    return out;
}

// ---- idle notify ------------------------------------------------------------------------------

struct IdleNotifier::Notification {
    Weak<ExtIdleNotificationV1> resource;
    uint32_t timeout_ms;
    bool input_only;  // v2: ignores inhibitors
    bool idle = false;
    wl_event_source* timer = nullptr;
};

IdleNotifier::IdleNotifier(wl_display* display, Seat& seat) : display_(display), seat_(seat) {
    global_ = Global::create<ExtIdleNotifierV1>(display, 2, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<ExtIdleNotifierV1>(client, version, id);
        if (!m)
            return;
        auto create = [this](ExtIdleNotifierV1* self, uint32_t id, uint32_t timeout, wl_resource* seat_res,
                             bool input_only) {
            auto* r = make<ExtIdleNotificationV1>(self->client(), self->version(), id);
            if (!r)
                return;
            if (Seat::from(seat_res) != &seat_) {
                r->detach();
                return;
            }
            auto owned = std::make_unique<Notification>(Notification{r, timeout, input_only});
            Notification* n = owned.get();
            n->timer = wl_event_loop_add_timer(wl_display_get_event_loop(display_), [](void* data) {
                auto* n = static_cast<Notification*>(data);
                if (!n->idle)
                    if (ExtIdleNotificationV1* r = n->resource.get()) {
                        n->idle = true;
                        r->send_idled();
                    }
                return 0;
            }, n);
            notifications_.push_back(std::move(owned));
            r->on_gone([this, n] {
                wl_event_source_remove(n->timer);
                std::erase_if(notifications_, [n](const auto& x) { return x.get() == n; });
            });
            arm(n);
        };
        m->on_get_idle_notification([create](ExtIdleNotifierV1* self, uint32_t id, uint32_t timeout,
                                             wl_resource* seat) { create(self, id, timeout, seat, false); });
        m->on_get_input_idle_notification([create](ExtIdleNotifierV1* self, uint32_t id, uint32_t timeout,
                                                   wl_resource* seat) { create(self, id, timeout, seat, true); });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

IdleNotifier::~IdleNotifier() {
    global_.reset();
    detach_all(managers_);
    for (auto& n : notifications_) {
        wl_event_source_remove(n->timer);
        if (auto* r = n->resource.get())
            r->detach();
    }
}

// Counting down, unless held off (an inhibitor, for the kind that minds).
void IdleNotifier::arm(Notification* n) {
    if (inhibited_ && !n->input_only)
        wl_event_source_timer_update(n->timer, 0);
    else
        wl_event_source_timer_update(n->timer, int(std::max<uint32_t>(n->timeout_ms, 1)));
}

void IdleNotifier::activity() {
    for (auto& n : notifications_) {
        if (n->idle)
            if (ExtIdleNotificationV1* r = n->resource.get()) {
                n->idle = false;
                r->send_resumed();
            }
        arm(n.get());
    }
}

void IdleNotifier::set_inhibited(bool inhibited) {
    if (inhibited == inhibited_)
        return;
    inhibited_ = inhibited;
    for (auto& n : notifications_)
        if (!n->input_only && !n->idle)
            arm(n.get());
}

// ---- pointer warp -------------------------------------------------------------------------------

PointerWarps::PointerWarps(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<WpPointerWarpV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<WpPointerWarpV1>(client, version, id);
        if (!m)
            return;
        m->on_warp_pointer([this](WpPointerWarpV1*, wl_resource* surface_res, wl_resource* pointer, double x,
                                  double y, uint32_t serial) {
            Surface* s = Surface::from(surface_res);
            if (s && seat_.has_pointer(pointer))
                request_warp.emit({s, x, y, serial});
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

PointerWarps::~PointerWarps() {
    global_.reset();
    detach_all(managers_);
}

} // namespace atrium::wl
