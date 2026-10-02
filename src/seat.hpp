#pragma once
#include "config.hpp"
#include "scene/scene.hpp"
#include "listener.hpp"
#include "shake.hpp"
#include "wl/data_device.hpp"
#include "wl/ime.hpp"
#include "wl/input_ext.hpp"

#include <array>
#include <string>
#include <unordered_map>
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
    Listener<> keymap;
    // A virtual keyboard's own: the device the client types through.
    std::unique_ptr<wlr_keyboard> device;
    std::vector<wl::Connection> connections;
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
    void keyboard_enter(wl::Surface* surface);
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
#ifdef ATRIUM_XWAYLAND
    // X11 windows' pointer, when they set none: the theme's arrow.
    void set_x11_cursor();
#endif
    // The keyboard whose keymap and modifiers clients get.
    void use_keyboard(wlr_keyboard* kb, bool force = false);

    Server& server;
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
    void add_keyboard(wlr_keyboard* keyboard);
    void add_pointer(wlr_pointer* pointer);
    void update_capabilities();
    void configure_libinput(libinput_device* device);

    void key(KeyboardGroup& group, wlr_keyboard_key_event* event);
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
    void button(wlr_pointer_button_event* event);
    void axis(wlr_pointer_axis_event* event);
    double space_scroll_ = 0;  // Mod + scroll, toward the next space step
    void pointer_focus(View* view, wl::Surface* surface, double sx, double sy, uint32_t time);

    void activate_constraint(wl::PointerConstraints::Constraint* constraint);
    void warp_to_constraint_hint();
    // A client's pointer image: its surface, shown as the cursor.
    void set_cursor_surface(wl::Surface* surface, int hot_x, int hot_y);
    void start_drag(wl::Drag* drag);
    void new_virtual_keyboard(wl::VirtualInputs::Keyboard* vk);
    void new_virtual_pointer(wl::VirtualInputs::Pointer* vp);

    friend struct KeyboardGroup;

    std::unique_ptr<KeyboardGroup> keyboards_;
    std::vector<std::unique_ptr<KeyboardGroup>> virtual_keyboards_;
    std::array<bool, KEY_MAX + 1> consumed_{};  // keycodes whose press ran a binding
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
    const char* edge_reached_ = nullptr;  // screen edge the pointer rests on over a fullscreen app
    std::string edge_output_;             // ... and its screen's name
    void reach_edge();
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

    // The physical keyboards, each told when the layout is picked for them
    // (the group copies its members' state).
    struct PhysicalKeyboard {
        wlr_keyboard* wlr;
        Listener<> destroy;
    };
    std::vector<std::unique_ptr<PhysicalKeyboard>> physical_;
    uint32_t last_layout_ = 0;

    wl::PointerConstraints::Constraint* active_constraint_ = nullptr;

    wlr_keyboard* seat_kb_ = nullptr;  // see use_keyboard
    std::string sent_keymap_;
    wl::Surface* cursor_surface_ = nullptr;
    int cursor_hot_x_ = 0, cursor_hot_y_ = 0;
    wl::Connection cursor_commit_, cursor_gone_;
    scene::Node* drag_icon_ = nullptr;
    Listener<> drag_icon_gone_;
    wl::Connection drag_ended_;
    struct VirtualPointer;
    std::vector<std::unique_ptr<VirtualPointer>> virtual_pointers_;

    Listener<wlr_input_device> new_input_;
    Listener<wlr_pointer_motion_event> cursor_motion_;
    Listener<wlr_pointer_motion_absolute_event> cursor_motion_absolute_;
    Listener<wlr_pointer_button_event> cursor_button_;
    Listener<wlr_pointer_axis_event> cursor_axis_;
    Listener<> cursor_frame_;
    std::vector<wl::Connection> connections_;
};

} // namespace atrium
