#pragma once
#include "access_keys_core.hpp"
#include "config.hpp"
#include "listener.hpp"
#include "shake.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <map>
#include <memory>
#include <vector>

namespace atrium {

class Server;
class Titlebar;
class View;
struct Hit;

// All keyboards on the seat share one xkb state through a keyboard group.
// Virtual keyboards get a group each, so an on-screen or synthetic keyboard
// never inherits modifiers held on the physical one.
struct KeyboardGroup {
    KeyboardGroup(class Seat& seat, bool is_virtual);
    ~KeyboardGroup();

    class Seat& seat;
    const bool is_virtual;
    wl_client* owner = nullptr;  // a virtual keyboard's client
    wlr_keyboard_group* group = nullptr;
    wl_event_source* repeat_source = nullptr;
    xkb_keysym_t syms[2]{};  // level 0 and level 1 of the last key pressed
    uint32_t mods = 0;

    Listener<wlr_keyboard_key_event> key;
    Listener<> modifiers;
    Listener<> destroy;  // virtual keyboards only
};

class Seat {
public:
    explicit Seat(Server& server);
    ~Seat();
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    // The backend is being destroyed under us: stop listening to it.
    void backend_gone() { new_input_.disconnect(); }

    // The physical keyboards, as one (what an input method grabs).
    wlr_keyboard* physical_keyboard() const;
    uint32_t held_modifiers() const;

    enum class Mode { Normal, Pressed, Move, Resize };

    // Keyboard focus with held keys filtered: a key that triggered a binding
    // must not reach the newly focused client as "still held".
    void keyboard_enter(wlr_surface* surface);
    void clear_keyboard_focus();

    // Re-evaluate what is under the cursor without it having moved
    // (after a window maps, moves, closes or changes stacking).
    void refresh_pointer() { motion(0, nullptr, 0, 0, 0, 0); }

    void begin_move(View* view);
    void begin_resize(View* view, uint32_t edges);
    void cancel_grab();
    void view_unmapped(View* view);
    void titlebar_gone(Titlebar* bar);

    void apply_keyboard_config();
    // Keyboard layouts: the one in use (from 0), and each one's name.
    uint32_t layout() const;
    std::vector<std::string> layout_names() const;
    void set_layout(uint32_t index);
    void apply_pointer_config();
    // Pointing devices plugged in now.
    std::vector<wlr_pointer*> pointer_devices() const;
    void apply_cursor_theme();
    void set_default_cursor();
    // A device atrium makes itself (remote control's), taken as if plugged in.
    void add_virtual(wlr_input_device* device) { new_input(device); }

    Server& server;
    wlr_seat* wlr = nullptr;
    wlr_cursor* cursor = nullptr;
    wlr_xcursor_manager* xcursor = nullptr;
    Mode mode = Mode::Normal;

private:
    // Shaking the pointer grows the arrow for a moment, to find it.
    void shake_grow();
    void shake_settle();
    void show_shake_level(int level);
    ShakeDetector shake_;
    static constexpr int kShakeLevels = 4;  // sizes on the way up: 1.5x to 3x
    std::array<wlr_xcursor_manager*, kShakeLevels> shake_xcursor_{};
    int shake_level_ = 0;  // 0: normal size
    bool shaking_ = false;
    wl_event_source* shake_end_ = nullptr;

    void new_input(wlr_input_device* device);
    void add_switch(wlr_switch* sw);
    void add_keyboard(wlr_keyboard* keyboard);
    void add_pointer(wlr_pointer* pointer);

    // touch_tablet.cpp: touchscreens, drawing tablets and their pads, as
    // labwc has them (touch.c, tablet.c, tablet-pad.c). An app that speaks
    // touch or the tablet protocol gets them; anywhere else they work the
    // pointer (a touch or the pen's tip is a left click).
    void add_touch(wlr_touch* touch);
    void add_tablet(wlr_tablet* tablet);
    void add_tablet_pad(wlr_tablet_pad* pad);
    void touch_down(wlr_touch_down_event* e);
    void touch_up(wlr_touch_up_event* e);
    void touch_motion(wlr_touch_motion_event* e);
    void tool_proximity(wlr_tablet_tool_proximity_event* e);
    void tool_axis(wlr_tablet_tool_axis_event* e);
    void tool_tip(wlr_tablet_tool_tip_event* e);
    void tool_button(wlr_tablet_tool_button_event* e);
    void emulate_absolute(wlr_input_device* device, double x, double y, uint32_t time);
    void emulate_button(uint32_t button, bool pressed, uint32_t time);
    void focus_for_touch(const struct Hit& hit);
    struct TabletTool* tool_for(wlr_tablet_tool* tool);
    // Where a pen or a touch lands: the surface if it takes that kind of
    // input, with the layout-to-surface offset.
    wlr_surface* touch_target(double lx, double ly, double& ox, double& oy, bool tablet);
public:
    void pads_enter(wlr_surface* surface);  // keyboard focus moved
    void map_to_outputs();  // each touchscreen to its own screen
private:
    void update_capabilities();
    void configure_libinput(libinput_device* device);

    // Typing aids first (sticky, slow and bounce keys), then key_through().
    void key(KeyboardGroup& group, wlr_keyboard_key_event* event);
    void key_through(KeyboardGroup& group, wlr_keyboard_key_event* event);
    void sticky_key(KeyboardGroup& group, uint32_t keycode, bool pressed);
    // Sticky keys' latched and locked modifiers into the keyboard's state
    // (and so to the focused app).
    void apply_sticky();
    // A snippet keyword being typed; true when this key completed one (and
    // the shell was asked to type the snippet instead of it).
    bool watch_keyword(wlr_keyboard* kb, uint32_t keycode, xkb_keysym_t sym);
    void modifiers(KeyboardGroup& group);
    int key_repeat(KeyboardGroup& group);
    // While locked, only bindings marked for the lock screen.
    const Keybind* find_binding(uint32_t mods, xkb_keysym_t sym) const;
    bool shortcuts_inhibited() const;

    void motion(uint32_t time, wlr_input_device* device, double dx, double dy,
                double dx_unaccel, double dy_unaccel);
    // A button as pointer.buttons remaps it, then on to button_event.
    void button(wlr_pointer_button_event* event);
    void button_event(wlr_pointer_button_event* event);
    void axis(wlr_pointer_axis_event* event);
    double space_scroll_ = 0;  // Mod + scroll, toward the next space step
    void pointer_focus(View* view, wlr_surface* surface, double sx, double sy, uint32_t time);

    void new_constraint(wlr_pointer_constraint_v1* constraint);
    void activate_constraint(wlr_pointer_constraint_v1* constraint);
    void warp_to_constraint_hint();

    friend struct KeyboardGroup;

    std::unique_ptr<KeyboardGroup> keyboards_;

    struct TouchPoint {
        int32_t id;
        wlr_surface* surface;  // null: working the pointer
        double ox, oy;         // layout minus surface coordinates at the touch
    };
    std::vector<TouchPoint> touch_points_;
    std::vector<std::unique_ptr<struct InputDevice>> mapped_;  // touchscreens and tablets
    std::vector<std::unique_ptr<struct TabletDevice>> tablets_;
    std::vector<std::unique_ptr<struct TabletPad>> pads_;
    std::vector<std::unique_ptr<struct TabletTool>> tools_;
    bool pen_emulating_ = false;  // a tip or button press is working the pointer
    Listener<wlr_touch_down_event> touch_down_;
    Listener<wlr_touch_up_event> touch_up_;
    Listener<wlr_touch_motion_event> touch_motion_;
    Listener<> touch_frame_;
    Listener<wlr_tablet_tool_proximity_event> tool_proximity_;
    Listener<wlr_tablet_tool_axis_event> tool_axis_;
    Listener<wlr_tablet_tool_tip_event> tool_tip_;
    Listener<wlr_tablet_tool_button_event> tool_button_;
    std::vector<std::unique_ptr<KeyboardGroup>> virtual_keyboards_;
    std::array<bool, KEY_MAX + 1> consumed_{};  // keycodes whose press ran a binding
    typing::BounceKeys bounce_;
    typing::SlowKeys slow_;
    typing::StickyKeys sticky_;
    // A slow key's press, held back until it has been down long enough.
    struct SlowPress {
        Seat* seat;
        KeyboardGroup* group;
        wlr_keyboard_key_event event;
        wl_event_source* timer = nullptr;
        ~SlowPress() {
            if (timer)
                wl_event_source_remove(timer);
        }
    };
    std::unordered_map<uint32_t, std::unique_ptr<SlowPress>> slow_presses_;
    std::unordered_map<uint32_t, std::string> portal_held_;  // keycode → portal shortcut held down

    View* grab_view_ = nullptr;
    double grab_x_ = 0, grab_y_ = 0;  // cursor at grab start
    wlr_box grab_geom_{};             // view geometry at grab start
    uint32_t grab_edges_ = 0;
    bool grab_unmaximize_ = false;  // moving a maximized/snapped window; restore once it drags
    uint32_t snap_zone_ = 0;        // snap zone under the cursor while moving
    // Pushing a dragged window against the left or right end of the screens
    // carries it to the space that way: how far the pointer pushed past the
    // edge, and whether it just did (no snapping until it leaves the edge).
    double edge_push_ = 0;
    bool edge_carried_ = false;
    void push_edge(double dx);
    void hot_corner(uint32_t time);
    const char* edge_reached_ = nullptr;  // screen edge the pointer rests on over a fullscreen app
    std::string edge_output_;             // ... and its screen's name
    // Arriving at an edge counts once the pointer has stayed there a moment.
    static constexpr int kEdgeHoldMs = 400;
    const char* edge_pending_ = nullptr;
    std::string edge_pending_output_;
    bool edge_held_ = false;
    wl_event_source* edge_timer_ = nullptr;
    void reach_edge();
    uint32_t mod_tap_ = 0;  // the Mod key pressed alone, a tap if released so
    bool is_mod_key(xkb_keysym_t sym) const;
    bool overview_press_ = false;   // a button went down on the overview
    bool switcher_press_ = false;   // ... or while the window switcher was up
    void unmaximize_for_drag();

    // Frame edges of decorated windows: a band just outside the frame that
    // resizes it. Null view when the point is not in any band.
    struct ResizeZone {
        View* view = nullptr;
        uint32_t edges = 0;
    };
    ResizeZone resize_zone(double lx, double ly, const Hit& hit) const;
    void set_titlebar_hover(Titlebar* bar, int part, int tab = -1);
    bool titlebar_button(wlr_pointer_button_event* e, const Hit& hit);

    Titlebar* hover_bar_ = nullptr;   // title bar showing hover state
    Titlebar* press_bar_ = nullptr;   // title bar whose button is held
    int press_part_ = 0;              // Titlebar::Part held down
    int press_tab_ = -1;              // ... on this tab (a tab's close button)

    // A tab held on its bar: dragged along it, it moves among the tabs;
    // pulled off it, it tears off into a window of its own, being moved.
    View* tab_drag_ = nullptr;
    double tab_scroll_ = 0;
    void tab_drag_motion();
    // A window being moved over another window's tab bar goes in there
    // when dropped: the bar shows where.
    struct TabDrop {
        View* into = nullptr;
        size_t slot = 0;
    };
    TabDrop tab_drop_at(View* dragged) const;
    Titlebar* drop_bar_ = nullptr;
    // The bar shows where; the window carried there fades, so it can be seen.
    void show_tab_drop(View* dragged, Titlebar* bar, int slot);
    View* last_bar_click_view_ = nullptr;
    uint32_t last_bar_click_ms_ = 0;

    struct PointerDevice {
        wlr_pointer* wlr;
        Listener<> destroy;
    };
    std::vector<std::unique_ptr<PointerDevice>> pointers_;

    // The physical keyboards, each told when the layout is picked for them
    // (the group copies its members' state).
    struct PhysicalKeyboard {
        wlr_keyboard* wlr;
        Listener<> destroy;
    };
    std::vector<std::unique_ptr<PhysicalKeyboard>> physical_;
    uint32_t last_layout_ = 0;

    // Remapped buttons held down: each keeps its remap, and the keys it
    // pressed, until it's let go. Their keys come from a keyboard of atrium's
    // own, so shortcuts see them as typed.
    std::map<uint32_t, ButtonRemap> remapped_;
    std::map<uint32_t, std::vector<uint32_t>> remap_keys_;
    std::unique_ptr<wlr_keyboard> remap_keyboard_;
    bool remap_typing_ = false;  // keys from a button don't hide the pointer
    void send_remap_keys(const std::vector<uint32_t>& keys, bool pressed, uint32_t time);

    wlr_pointer_constraint_v1* active_constraint_ = nullptr;
    struct Constraint;
    std::vector<std::unique_ptr<Constraint>> constraints_;

    Listener<wlr_input_device> new_input_;
    Listener<wlr_virtual_keyboard_v1> new_virtual_keyboard_;
    Listener<wlr_virtual_pointer_v1_new_pointer_event> new_virtual_pointer_;
    Listener<wlr_pointer_motion_event> cursor_motion_;
    Listener<wlr_pointer_motion_absolute_event> cursor_motion_absolute_;
    Listener<wlr_pointer_button_event> cursor_button_;
    Listener<wlr_pointer_axis_event> cursor_axis_;
    Listener<> cursor_frame_;
    Listener<wlr_pointer_swipe_begin_event> swipe_begin_;
    Listener<wlr_pointer_swipe_update_event> swipe_update_;
    Listener<wlr_pointer_swipe_end_event> swipe_end_;
    Listener<wlr_pointer_pinch_begin_event> pinch_begin_;
    Listener<wlr_pointer_pinch_update_event> pinch_update_;
    Listener<wlr_pointer_pinch_end_event> pinch_end_;
    Listener<wlr_pointer_hold_begin_event> hold_begin_;
    Listener<wlr_pointer_hold_end_event> hold_end_;
    bool typing_hidden_ = false;  // pointer.hide_while_typing
    Listener<wlr_seat_pointer_request_set_cursor_event> request_cursor_;
    Listener<wlr_cursor_shape_manager_v1_request_set_shape_event> request_cursor_shape_;
    Listener<wlr_seat_request_set_selection_event> request_selection_;
    Listener<wlr_seat_request_set_primary_selection_event> request_primary_selection_;
    Listener<wlr_seat_request_start_drag_event> request_start_drag_;
    Listener<wlr_drag> start_drag_;
    Listener<wlr_pointer_constraint_v1> new_constraint_;
};

} // namespace atrium
