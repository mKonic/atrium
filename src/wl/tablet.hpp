#pragma once
#include "wl/seat.hpp"

#include <optional>
#include <string>

namespace atrium::wl {

class ZwpTabletSeatV2;

// zwp_tablet_manager_v2: drawing tablets. Every client's tablet seat hears of
// each tablet, tool (a pen, its eraser end, an airbrush) and pad (the
// tablet's buttons, rings, strips and dials); the compositor feeds a tool's
// events to the surface it is over and a pad's to the surface it entered.
class Tablets {
public:
    struct TabletInfo {
        std::string name, path;
        uint32_t vid = 0, pid = 0;
        uint32_t bustype = 0;  // zwp_tablet_v2 bustype; 0: unknown
    };
    struct ToolInfo {
        uint32_t type = 0x140;  // zwp_tablet_tool_v2 type (pen)
        uint64_t hardware_serial = 0, hardware_id_wacom = 0;
        std::vector<uint32_t> capabilities;  // zwp_tablet_tool_v2 capability
    };
    struct PadGroup {
        std::vector<uint32_t> buttons;  // the pad's buttons this group owns
        int rings = 0, strips = 0, dials = 0;
        uint32_t modes = 0;
    };
    struct PadInfo {
        std::string path;
        uint32_t buttons = 0;
        std::vector<PadGroup> groups;  // rings, strips and dials are numbered across them, in order
    };

    struct Tablet;
    struct Tool;
    struct Pad;

    Tablets(wl_display* display, Seat& seat);
    ~Tablets();

    Tablet* add_tablet(TabletInfo info);
    void remove(Tablet* tablet);
    Tool* add_tool(ToolInfo info);
    void remove(Tool* tool);
    Pad* add_pad(PadInfo info);
    void remove(Pad* pad);

    // ---- tools: events up to frame() make one frame ----
    void proximity_in(Tool* tool, Tablet* tablet, Surface* surface, double sx, double sy);
    void proximity_out(Tool* tool);
    void down(Tool* tool);
    void up(Tool* tool);
    void motion(Tool* tool, double sx, double sy);
    void pressure(Tool* tool, double pressure);  // 0..1
    void distance(Tool* tool, double distance);  // 0..1
    void tilt(Tool* tool, double x_degrees, double y_degrees);
    void rotation(Tool* tool, double degrees);
    void slider(Tool* tool, double position);  // -1..1
    void wheel(Tool* tool, double degrees, int32_t clicks);
    void button(Tool* tool, uint32_t button, bool pressed);
    void frame(Tool* tool, uint32_t time_ms);
    Surface* focus(const Tool* tool) const;

    // ---- pads ----
    void pad_enter(Pad* pad, Tablet* tablet, Surface* surface);
    void pad_leave(Pad* pad);
    void pad_button(Pad* pad, uint32_t time_ms, uint32_t button, bool pressed);
    // nullopt: the finger left (a stop event).
    void pad_ring(Pad* pad, size_t ring, std::optional<double> degrees, bool finger, uint32_t time_ms);
    void pad_strip(Pad* pad, size_t strip, std::optional<double> position, bool finger, uint32_t time_ms);  // 0..1
    void pad_dial(Pad* pad, size_t dial, int32_t value120, uint32_t time_ms);
    void pad_mode(Pad* pad, size_t group, uint32_t mode, uint32_t time_ms);
    Surface* focus(const Pad* pad) const;

    // A client set a tool's image (null surface: hide it).
    struct CursorRequest {
        Tool* tool;
        wl_client* client;
        Surface* surface;
        int32_t hotspot_x, hotspot_y;
    };
    // A client named what a pad control does, for an on-screen overlay.
    struct Feedback {
        Pad* pad;
        enum class Kind { Button, Ring, Strip, Dial } kind;
        size_t index;
        std::string description;
    };
    struct {
        Signal<const CursorRequest&> request_cursor;
        Signal<const Feedback&> feedback;
    } events;

private:
    void announce(ZwpTabletSeatV2* seat, Tablet* tablet);
    void announce(ZwpTabletSeatV2* seat, Tool* tool);
    void announce(ZwpTabletSeatV2* seat, Pad* pad);
    template <class Fn>
    void each_focused(Tool* tool, Fn fn);

    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<Weak<ZwpTabletSeatV2>> seats_;
    std::vector<std::unique_ptr<Tablet>> tablets_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::vector<std::unique_ptr<Pad>> pads_;
};

} // namespace atrium::wl
