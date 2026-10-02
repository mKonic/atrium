#include "wl/tablet.hpp"

#include "tablet-v2-server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace atrium::wl {

namespace {

// A tool's image: a surface a client set with zwp_tablet_tool_v2.set_cursor.
class ToolCursorRole : public Role {
public:
    static constexpr const char* kName = "zwp_tablet_tool_v2-cursor";
    const char* name() const override { return kName; }
};
ToolCursorRole g_tool_cursor_role;

uint32_t now_ms() {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

bool live(Resource* r) {
    return r && !r->inert();
}

// The 0..65535 scale pressure, distance and strips go out on.
uint32_t unit(double v) {
    return uint32_t(std::lround(std::clamp(v, 0.0, 1.0) * 65535));
}

} // namespace

// One object per tablet seat (so per client) that heard of the device.
template <class T>
struct Bound {
    Weak<ZwpTabletSeatV2> seat;
    Weak<T> resource;
};

struct Tablets::Tablet {
    TabletInfo info;
    std::vector<Bound<ZwpTabletV2>> bound;

    ZwpTabletV2* for_seat(ZwpTabletSeatV2* seat) const {
        for (const auto& b : bound)
            if (b.seat.get() == seat && live(b.resource.get()))
                return b.resource.get();
        return nullptr;
    }
};

struct Tablets::Tool {
    ToolInfo info;
    std::vector<Bound<ZwpTabletToolV2>> bound;
    Surface* focus = nullptr;
    Tablet* tablet = nullptr;
    uint32_t proximity_serial = 0;
    bool is_down = false;
    Connection focus_gone;
};

struct Tablets::Pad {
    PadInfo info;
    struct PadBound {
        Weak<ZwpTabletSeatV2> seat;
        Weak<ZwpTabletPadV2> resource;
        std::vector<Weak<ZwpTabletPadGroupV2>> groups;
        std::vector<Weak<ZwpTabletPadRingV2>> rings;
        std::vector<Weak<ZwpTabletPadStripV2>> strips;
        std::vector<Weak<ZwpTabletPadDialV2>> dials;
    };
    std::vector<PadBound> bound;
    Surface* focus = nullptr;
    Tablet* tablet = nullptr;
    Connection focus_gone;

    template <class Fn>
    void each_focused(Fn fn) {
        if (!focus)
            return;
        for (auto& b : bound)
            if (live(b.resource.get()) && b.resource->client() == focus->client())
                fn(b);
    }
};

Tablets::Tablets(wl_display* display, Seat& seat) : seat_(seat) {
    global_ = Global::create<ZwpTabletManagerV2>(display, 2, [this](wl_client* client, uint32_t version,
                                                                    uint32_t id) {
        auto* m = make<ZwpTabletManagerV2>(client, version, id);
        if (!m)
            return;
        m->on_get_tablet_seat([this](ZwpTabletManagerV2* self, uint32_t id, wl_resource* seat_res) {
            auto* s = make<ZwpTabletSeatV2>(self->client(), self->version(), id);
            if (!s)
                return;
            if (Seat::from(seat_res) != &seat_) {
                s->detach();
                return;
            }
            std::erase_if(seats_, [](const auto& w) { return !w; });
            seats_.push_back(s);
            for (auto& t : tablets_)
                announce(s, t.get());
            for (auto& t : tools_)
                announce(s, t.get());
            for (auto& p : pads_)
                announce(s, p.get());
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Tablets::~Tablets() {
    global_.reset();
    for (auto& w : managers_)
        if (w)
            w->detach();
    for (auto& w : seats_)
        if (w)
            w->detach();
    for (auto& t : tablets_)
        for (auto& b : t->bound)
            if (b.resource)
                b.resource->detach();
    for (auto& t : tools_)
        for (auto& b : t->bound)
            if (b.resource)
                b.resource->detach();
    for (auto& p : pads_)
        for (auto& b : p->bound) {
            if (b.resource)
                b.resource->detach();
            for (auto& r : b.rings)
                if (r)
                    r->detach();
            for (auto& r : b.strips)
                if (r)
                    r->detach();
            for (auto& r : b.dials)
                if (r)
                    r->detach();
        }
}

// ---- announcing -------------------------------------------------------------------------

void Tablets::announce(ZwpTabletSeatV2* seat, Tablet* tablet) {
    auto* r = make<ZwpTabletV2>(seat->client(), seat->version(), 0);
    if (!r)
        return;
    seat->send_tablet_added(r);
    const TabletInfo& i = tablet->info;
    if (!i.name.empty())
        r->send_name(i.name.c_str());
    if (i.vid || i.pid)
        r->send_id(i.vid, i.pid);
    if (!i.path.empty())
        r->send_path(i.path.c_str());
    if (i.bustype && r->version() >= 2)
        r->send_bustype(i.bustype);
    r->send_done();
    std::erase_if(tablet->bound, [](const auto& b) { return !b.resource; });
    tablet->bound.push_back({seat, r});
}

void Tablets::announce(ZwpTabletSeatV2* seat, Tool* tool) {
    auto* r = make<ZwpTabletToolV2>(seat->client(), seat->version(), 0);
    if (!r)
        return;
    seat->send_tool_added(r);
    const ToolInfo& i = tool->info;
    r->send_type(i.type);
    if (i.hardware_serial)
        r->send_hardware_serial(uint32_t(i.hardware_serial >> 32), uint32_t(i.hardware_serial));
    if (i.hardware_id_wacom)
        r->send_hardware_id_wacom(uint32_t(i.hardware_id_wacom >> 32), uint32_t(i.hardware_id_wacom));
    for (uint32_t c : i.capabilities)
        r->send_capability(c);
    r->send_done();
    r->on_set_cursor([this, tool](ZwpTabletToolV2* self, uint32_t serial, wl_resource* surface_res, int32_t hx,
                                  int32_t hy) {
        if (!tool->focus || tool->focus->client() != self->client() || serial != tool->proximity_serial)
            return;  // stale: the tool left, or is over another client
        Surface* surface = surface_res ? Surface::from(surface_res) : nullptr;
        if (surface && surface->role() != &g_tool_cursor_role &&
            !surface->set_role(&g_tool_cursor_role, self, uint32_t(ZwpTabletToolV2::Error::Role)))
            return;
        events.request_cursor.emit({tool, self->client(), surface, hx, hy});
    });
    std::erase_if(tool->bound, [](const auto& b) { return !b.resource; });
    tool->bound.push_back({seat, r});
}

void Tablets::announce(ZwpTabletSeatV2* seat, Pad* pad) {
    wl_client* client = seat->client();
    uint32_t version = seat->version();
    auto* r = make<ZwpTabletPadV2>(client, version, 0);
    if (!r)
        return;
    seat->send_pad_added(r);
    Pad::PadBound b{seat, r, {}, {}, {}, {}};
    auto feedback = [this, pad](Feedback::Kind kind, size_t index, const char* description) {
        events.feedback.emit({pad, kind, index, description ? description : ""});
    };
    r->on_set_feedback([feedback](ZwpTabletPadV2*, uint32_t button, const char* description, uint32_t) {
        feedback(Feedback::Kind::Button, button, description);
    });
    if (!pad->info.path.empty())
        r->send_path(pad->info.path.c_str());
    r->send_buttons(pad->info.buttons);
    for (const PadGroup& g : pad->info.groups) {
        auto* group = make<ZwpTabletPadGroupV2>(client, version, 0);
        if (!group)
            return;
        r->send_group(group);
        wl_array buttons;
        wl_array_init(&buttons);
        if (auto* p = static_cast<uint32_t*>(wl_array_add(&buttons, g.buttons.size() * sizeof(uint32_t))))
            std::ranges::copy(g.buttons, p);
        group->send_buttons(&buttons);
        wl_array_release(&buttons);
        for (int i = 0; i < g.rings; ++i)
            if (auto* ring = make<ZwpTabletPadRingV2>(client, version, 0)) {
                group->send_ring(ring);
                ring->on_set_feedback([feedback, n = b.rings.size()](ZwpTabletPadRingV2*, const char* d, uint32_t) {
                    feedback(Feedback::Kind::Ring, n, d);
                });
                b.rings.push_back(ring);
            }
        for (int i = 0; i < g.strips; ++i)
            if (auto* strip = make<ZwpTabletPadStripV2>(client, version, 0)) {
                group->send_strip(strip);
                strip->on_set_feedback([feedback, n = b.strips.size()](ZwpTabletPadStripV2*, const char* d,
                                                                       uint32_t) {
                    feedback(Feedback::Kind::Strip, n, d);
                });
                b.strips.push_back(strip);
            }
        if (version >= 2)
            for (int i = 0; i < g.dials; ++i)
                if (auto* dial = make<ZwpTabletPadDialV2>(client, version, 0)) {
                    group->send_dial(dial);
                    dial->on_set_feedback([feedback, n = b.dials.size()](ZwpTabletPadDialV2*, const char* d,
                                                                         uint32_t) {
                        feedback(Feedback::Kind::Dial, n, d);
                    });
                    b.dials.push_back(dial);
                }
        group->send_modes(g.modes);
        group->send_done();
        b.groups.push_back(group);
    }
    r->send_done();
    std::erase_if(pad->bound, [](const auto& x) { return !x.resource; });
    pad->bound.push_back(std::move(b));
}

// ---- devices ----------------------------------------------------------------------------

Tablets::Tablet* Tablets::add_tablet(TabletInfo info) {
    auto* t = tablets_.emplace_back(std::make_unique<Tablet>(Tablet{std::move(info), {}})).get();
    for (auto& s : seats_)
        if (live(s.get()))
            announce(s.get(), t);
    return t;
}

void Tablets::remove(Tablet* tablet) {
    for (auto& tool : tools_)
        if (tool->tablet == tablet)
            proximity_out(tool.get());
    for (auto& pad : pads_)
        if (pad->tablet == tablet)
            pad_leave(pad.get());
    for (auto& b : tablet->bound)
        if (live(b.resource.get())) {
            b.resource->send_removed();
            b.resource->detach();
        }
    std::erase_if(tablets_, [&](const auto& t) { return t.get() == tablet; });
}

Tablets::Tool* Tablets::add_tool(ToolInfo info) {
    auto* t = tools_.emplace_back(std::make_unique<Tool>()).get();
    t->info = std::move(info);
    for (auto& s : seats_)
        if (live(s.get()))
            announce(s.get(), t);
    return t;
}

void Tablets::remove(Tool* tool) {
    proximity_out(tool);
    for (auto& b : tool->bound)
        if (live(b.resource.get())) {
            b.resource->send_removed();
            b.resource->detach();
        }
    std::erase_if(tools_, [&](const auto& t) { return t.get() == tool; });
}

Tablets::Pad* Tablets::add_pad(PadInfo info) {
    auto* p = pads_.emplace_back(std::make_unique<Pad>()).get();
    p->info = std::move(info);
    for (auto& s : seats_)
        if (live(s.get()))
            announce(s.get(), p);
    return p;
}

void Tablets::remove(Pad* pad) {
    pad_leave(pad);
    for (auto& b : pad->bound) {
        if (live(b.resource.get())) {
            b.resource->send_removed();
            b.resource->detach();
        }
        for (auto& r : b.rings)
            if (r)
                r->detach();
        for (auto& r : b.strips)
            if (r)
                r->detach();
        for (auto& r : b.dials)
            if (r)
                r->detach();
    }
    std::erase_if(pads_, [&](const auto& p) { return p.get() == pad; });
}

// ---- tools ------------------------------------------------------------------------------

template <class Fn>
void Tablets::each_focused(Tool* tool, Fn fn) {
    if (!tool->focus)
        return;
    for (auto& b : tool->bound)
        if (live(b.resource.get()) && b.resource->client() == tool->focus->client())
            fn(b.resource.get());
}

bool Tablets::bound_by(const Tool* tool, wl_client* client) const {
    return std::ranges::any_of(tool->bound, [client](const auto& b) {
        return b.resource && b.resource->client() == client;
    });
}

void Tablets::proximity_in(Tool* tool, Tablet* tablet, Surface* surface, double sx, double sy) {
    if (tool->focus == surface && tool->tablet == tablet) {
        motion(tool, sx, sy);
        return;
    }
    if (tool->focus)
        proximity_out(tool);
    tool->focus = surface;
    tool->tablet = tablet;
    tool->proximity_serial = seat_.next_serial();
    tool->focus_gone = surface->events.destroy.connect([tool] {
        tool->focus = nullptr;
        tool->is_down = false;
        tool->focus_gone = {};
    });
    for (auto& b : tool->bound) {
        ZwpTabletToolV2* r = b.resource.get();
        if (!live(r) || r->client() != surface->client())
            continue;
        // The client's tablet object from the same tablet seat.
        if (ZwpTabletV2* t = tablet->for_seat(b.seat.get())) {
            r->send_proximity_in(tool->proximity_serial, t, surface->resource());
            r->send_motion(sx, sy);
        }
    }
}

void Tablets::proximity_out(Tool* tool) {
    if (!tool->focus)
        return;
    if (tool->is_down)
        up(tool);
    // proximity_out ends a frame of its own: the tool has no focus to send
    // the caller's frame() to after it.
    uint32_t time = now_ms();
    each_focused(tool, [&](ZwpTabletToolV2* r) {
        r->send_proximity_out();
        r->send_frame(time);
    });
    tool->focus_gone = {};
    tool->tablet = nullptr;
    tool->focus = nullptr;
    tool->proximity_serial = 0;
}

void Tablets::down(Tool* tool) {
    if (!tool->focus || tool->is_down)
        return;
    tool->is_down = true;
    uint32_t serial = seat_.next_serial();
    seat_.remember_serial(tool->focus->client(), serial);
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_down(serial); });
}

void Tablets::up(Tool* tool) {
    if (!tool->is_down)
        return;
    tool->is_down = false;
    each_focused(tool, [](ZwpTabletToolV2* r) { r->send_up(); });
}

void Tablets::motion(Tool* tool, double sx, double sy) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_motion(sx, sy); });
}

void Tablets::pressure(Tool* tool, double p) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_pressure(unit(p)); });
}

void Tablets::distance(Tool* tool, double d) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_distance(unit(d)); });
}

void Tablets::tilt(Tool* tool, double x, double y) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_tilt(x, y); });
}

void Tablets::rotation(Tool* tool, double degrees) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_rotation(degrees); });
}

void Tablets::slider(Tool* tool, double position) {
    auto v = int32_t(std::lround(std::clamp(position, -1.0, 1.0) * 65535));
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_slider(v); });
}

void Tablets::wheel(Tool* tool, double degrees, int32_t clicks) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_wheel(degrees, clicks); });
}

void Tablets::button(Tool* tool, uint32_t button, bool pressed) {
    if (!tool->focus)
        return;
    uint32_t serial = seat_.next_serial();
    if (pressed)
        seat_.remember_serial(tool->focus->client(), serial);
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_button(serial, button, pressed ? 1 : 0); });
}

void Tablets::frame(Tool* tool, uint32_t time_ms) {
    each_focused(tool, [&](ZwpTabletToolV2* r) { r->send_frame(time_ms); });
}

Surface* Tablets::focus(const Tool* tool) const {
    return tool->focus;
}

// ---- pads -------------------------------------------------------------------------------

void Tablets::pad_enter(Pad* pad, Tablet* tablet, Surface* surface) {
    if (pad->focus == surface && pad->tablet == tablet)
        return;
    pad_leave(pad);
    pad->focus = surface;
    pad->tablet = tablet;
    pad->focus_gone = surface->events.destroy.connect([pad] {
        pad->focus = nullptr;
        pad->tablet = nullptr;
        pad->focus_gone = {};
    });
    uint32_t serial = seat_.next_serial();
    pad->each_focused([&](Pad::PadBound& b) {
        if (ZwpTabletV2* t = tablet->for_seat(b.seat.get()))
            b.resource->send_enter(serial, t, surface->resource());
    });
}

void Tablets::pad_leave(Pad* pad) {
    if (!pad->focus)
        return;
    uint32_t serial = seat_.next_serial();
    pad->each_focused([&](Pad::PadBound& b) { b.resource->send_leave(serial, pad->focus->resource()); });
    pad->focus_gone = {};
    pad->focus = nullptr;
    pad->tablet = nullptr;
}

void Tablets::pad_button(Pad* pad, uint32_t time_ms, uint32_t button, bool pressed) {
    pad->each_focused([&](Pad::PadBound& b) { b.resource->send_button(time_ms, button, pressed ? 1 : 0); });
}

void Tablets::pad_ring(Pad* pad, size_t ring, std::optional<double> degrees, bool finger, uint32_t time_ms) {
    pad->each_focused([&](Pad::PadBound& b) {
        ZwpTabletPadRingV2* r = ring < b.rings.size() ? b.rings[ring].get() : nullptr;
        if (!live(r))
            return;
        if (finger)
            r->send_source(uint32_t(ZwpTabletPadRingV2::Source::Finger));
        if (degrees)
            r->send_angle(*degrees);
        else
            r->send_stop();
        r->send_frame(time_ms);
    });
}

void Tablets::pad_strip(Pad* pad, size_t strip, std::optional<double> position, bool finger, uint32_t time_ms) {
    pad->each_focused([&](Pad::PadBound& b) {
        ZwpTabletPadStripV2* s = strip < b.strips.size() ? b.strips[strip].get() : nullptr;
        if (!live(s))
            return;
        if (finger)
            s->send_source(uint32_t(ZwpTabletPadStripV2::Source::Finger));
        if (position)
            s->send_position(unit(*position));
        else
            s->send_stop();
        s->send_frame(time_ms);
    });
}

void Tablets::pad_dial(Pad* pad, size_t dial, int32_t value120, uint32_t time_ms) {
    pad->each_focused([&](Pad::PadBound& b) {
        ZwpTabletPadDialV2* d = dial < b.dials.size() ? b.dials[dial].get() : nullptr;
        if (!live(d))
            return;
        d->send_delta(value120);
        d->send_frame(time_ms);
    });
}

void Tablets::pad_mode(Pad* pad, size_t group, uint32_t mode, uint32_t time_ms) {
    uint32_t serial = seat_.next_serial();
    pad->each_focused([&](Pad::PadBound& b) {
        if (ZwpTabletPadGroupV2* g = group < b.groups.size() ? b.groups[group].get() : nullptr; live(g))
            g->send_mode_switch(time_ms, serial, mode);
    });
}

Surface* Tablets::focus(const Pad* pad) const {
    return pad->focus;
}

} // namespace atrium::wl
