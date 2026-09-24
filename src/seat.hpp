#pragma once
#include "config.hpp"
#include "listener.hpp"

#include <array>
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
    void apply_pointer_config();
    void apply_cursor_theme();
    void set_default_cursor();

    Server& server;
    wlr_seat* wlr = nullptr;
    wlr_cursor* cursor = nullptr;
    wlr_xcursor_manager* xcursor = nullptr;
    Mode mode = Mode::Normal;

private:
    void new_input(wlr_input_device* device);
    void add_keyboard(wlr_keyboard* keyboard);
    void add_pointer(wlr_pointer* pointer);
    void update_capabilities();
    void configure_libinput(libinput_device* device);

    void key(KeyboardGroup& group, wlr_keyboard_key_event* event);
    void modifiers(KeyboardGroup& group);
    int key_repeat(KeyboardGroup& group);
    // While locked, only bindings marked for the lock screen.
    const Keybind* find_binding(uint32_t mods, xkb_keysym_t sym) const;
    bool shortcuts_inhibited() const;

    void motion(uint32_t time, wlr_input_device* device, double dx, double dy,
                double dx_unaccel, double dy_unaccel);
    void button(wlr_pointer_button_event* event);
    void axis(wlr_pointer_axis_event* event);
    void pointer_focus(View* view, wlr_surface* surface, double sx, double sy, uint32_t time);

    void new_constraint(wlr_pointer_constraint_v1* constraint);
    void activate_constraint(wlr_pointer_constraint_v1* constraint);
    void warp_to_constraint_hint();

    friend struct KeyboardGroup;

    std::unique_ptr<KeyboardGroup> keyboards_;
    std::vector<std::unique_ptr<KeyboardGroup>> virtual_keyboards_;
    std::array<bool, KEY_MAX + 1> consumed_{};  // keycodes whose press ran a binding

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
    void set_titlebar_hover(Titlebar* bar, int part);
    bool titlebar_button(wlr_pointer_button_event* e, const Hit& hit);

    Titlebar* hover_bar_ = nullptr;   // title bar showing hover state
    Titlebar* press_bar_ = nullptr;   // title bar whose button is held
    int press_part_ = 0;              // Titlebar::Part held down
    View* last_bar_click_view_ = nullptr;
    uint32_t last_bar_click_ms_ = 0;

    struct PointerDevice {
        wlr_pointer* wlr;
        Listener<> destroy;
    };
    std::vector<std::unique_ptr<PointerDevice>> pointers_;

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
    Listener<wlr_seat_pointer_request_set_cursor_event> request_cursor_;
    Listener<wlr_cursor_shape_manager_v1_request_set_shape_event> request_cursor_shape_;
    Listener<wlr_seat_request_set_selection_event> request_selection_;
    Listener<wlr_seat_request_set_primary_selection_event> request_primary_selection_;
    Listener<wlr_seat_request_start_drag_event> request_start_drag_;
    Listener<wlr_drag> start_drag_;
    Listener<wlr_pointer_constraint_v1> new_constraint_;
};

} // namespace atrium
