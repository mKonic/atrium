#pragma once
#include "config.hpp"
#include "input/keys.hpp"
#include "input/libinput.hpp"
#include "scene/scene.hpp"
#include "listener.hpp"
#include "shake.hpp"
#include "wl/data_device.hpp"
#include "wl/ime.hpp"
#include "wl/input_ext.hpp"
#include "util/xcursor.hpp"
#include "wl/tablet.hpp"

#include <array>
#include <list>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace atrium {

class Cursor;

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
    input::Keys keys;
    wl_event_source* repeat_source = nullptr;
    xkb_keysym_t syms[2]{};  // level 0 and level 1 of the last key pressed
    uint32_t mods = 0;
    std::vector<wl::Connection> connections;  // a virtual keyboard's
};

// A pointer button or a scroll, from whichever device.
struct ButtonEvent {
    uint32_t time_ms;
    uint32_t button;
    bool pressed;
};
struct AxisEvent {
    uint32_t time_ms;
    uint32_t orientation;  // wl_pointer.axis
    double delta;
    int32_t value120;      // wheel clicks x120 (0: none)
    uint32_t source;       // wl_pointer.axis_source
    bool inverted;         // natural scrolling
};

class Seat : private input::Libinput::Handler {
public:
    explicit Seat(Server& server);
    ~Seat();
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    // The lid is closed over this screen (it stays off whatever else wakes).
    bool lid_keeps_off(const class Output* output) const;

    // The backend is being destroyed under us: stop listening to it.
    void backend_gone() {
        host_input_.clear();
    }

    // The physical keyboards, as one (what an input method grabs).
    KeyboardGroup* physical_keyboard() const { return keyboards_.get(); }
    uint32_t held_modifiers() const;

    enum class Mode { Normal, Pressed, Move, Resize };

    // Keyboard focus with held keys filtered: a key that triggered a binding
    // must not reach the newly focused client as "still held".
    void keyboard_enter(wl::Surface* surface);
    void clear_keyboard_focus();

    // Re-evaluate what is under the cursor without it having moved
    // (after a window maps, moves, closes or changes stacking).
    void refresh_pointer() { motion(0, 0, 0, 0, 0); }

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
    // Pointing devices plugged in now, with their libinput device (null
    // nested).
    std::vector<std::pair<std::string, libinput_device*>> pointer_devices() const;
    // The session switched VT (true: back to ours).
    void session_active(bool active);
    void apply_cursor_theme();
    void set_default_cursor();
#ifdef ATRIUM_XWAYLAND
    // X11 windows' pointer, when they set none: the theme's arrow.
    void set_x11_cursor();
#endif
    // The keyboard whose keymap and modifiers clients get.
    void use_keyboard(KeyboardGroup* kb, bool force = false);

    Server& server;
    Cursor* cursor = nullptr;  // the server's
    std::unique_ptr<xcursor::Manager> xcursor;
    Mode mode = Mode::Normal;

private:
    // Shaking the pointer grows the arrow for a moment, to find it.
    void shake_grow();
    void shake_settle();
    void show_shake_level(int level);
    ShakeDetector shake_;
    static constexpr int kShakeLevels = 4;  // sizes on the way up: 1.5x to 3x
    std::array<std::unique_ptr<xcursor::Manager>, kShakeLevels> shake_xcursor_;
    int shake_level_ = 0;  // 0: normal size
    bool shaking_ = false;
    wl_event_source* shake_end_ = nullptr;


    // libinput's devices and events (seat_input.cpp).
    void device_added(input::Device& d) override;
    void device_removed(input::Device& d) override;
    void key(input::Device& d, uint32_t time_ms, uint32_t keycode, bool pressed) override;
    void motion(input::Device& d, uint32_t time_ms, double dx, double dy, double dx_unaccel,
                double dy_unaccel) override;
    void motion_absolute(input::Device& d, uint32_t time_ms, double x, double y) override;
    void button(input::Device& d, uint32_t time_ms, uint32_t button, bool pressed) override;
    void scroll(input::Device& d, const input::Scroll& s) override;
    void frame(input::Device& d) override;
    void swipe(input::Device& d, libinput_event_gesture* e, libinput_event_type type) override;
    void pinch(input::Device& d, libinput_event_gesture* e, libinput_event_type type) override;
    void hold(input::Device& d, libinput_event_gesture* e, libinput_event_type type) override;
    void touch(input::Device& d, libinput_event_touch* e, libinput_event_type type) override;
    void tablet_tool(input::Device& d, libinput_event_tablet_tool* e, libinput_event_type type) override;
    void tablet_pad(input::Device& d, libinput_event_tablet_pad* e, libinput_event_type type) override;
    void toggle(input::Device& d, libinput_switch which, bool on) override;
    // A point 0..1 on the device's screen (or all of them) in the layout.
    void to_layout(const input::Device& d, double x, double y, double* lx, double* ly) const;
    // A touch or tablet's point: the surface there, and where on it.
    struct SurfaceAt {
        wl::Surface* surface = nullptr;
        double sx = 0, sy = 0;
    };
    SurfaceAt surface_at(double lx, double ly) const;
    void update_capabilities();
    void configure_libinput(libinput_device* device);

    void key(KeyboardGroup& group, uint32_t time_ms, uint32_t keycode, bool pressed);
    // A snippet keyword being typed; true when this key completed one (and
    // the shell was asked to type the snippet instead of it).
    bool watch_keyword(KeyboardGroup& g, uint32_t keycode, xkb_keysym_t sym);
    void modifiers(KeyboardGroup& group);
    int key_repeat(KeyboardGroup& group);
    // While locked, only bindings marked for the lock screen.
    const Keybind* find_binding(uint32_t mods, xkb_keysym_t sym) const;
    bool shortcuts_inhibited() const;

    void motion(uint32_t time, double dx, double dy,
                double dx_unaccel, double dy_unaccel);
    void motion_absolute(uint32_t time, double lx, double ly);
    void button(const ButtonEvent& event);
    void axis(const AxisEvent& event);
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
    Box grab_geom_{};             // view geometry at grab start
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
    bool titlebar_button(const ButtonEvent& e, const Hit& hit);

    Titlebar* hover_bar_ = nullptr;   // title bar showing hover state
    Titlebar* press_bar_ = nullptr;   // title bar whose button is held
    int press_part_ = 0;              // Titlebar::Part held down
    View* last_bar_click_view_ = nullptr;
    uint32_t last_bar_click_ms_ = 0;

    uint32_t last_layout_ = 0;

    // libinput's, on a real session.
    std::unique_ptr<input::Libinput> libinput_;
    size_t libinput_keyboards_ = 0;
    // A swipe in progress: how far it went, with how many fingers.
    struct Swipe {
        uint32_t fingers = 0;
        double dx = 0, dy = 0;
        bool ours = false;  // atrium's (spaces, overview), or the client's
    } swipe_;
    // Touch points down, by id: what they touched.
    std::unordered_map<int32_t, wl::Surface*> touches_;
    // Tablets' and tools' protocol objects, by libinput's.
    std::unordered_map<libinput_tablet_tool*, wl::Tablets::Tool*> tools_;
    bool lid_closed_ = false;

    wl::PointerConstraints::Constraint* active_constraint_ = nullptr;

    KeyboardGroup* seat_kb_ = nullptr;  // see use_keyboard
    std::string sent_keymap_;
    wl::Surface* cursor_surface_ = nullptr;
    int cursor_hot_x_ = 0, cursor_hot_y_ = 0;
    wl::Connection cursor_commit_, cursor_gone_;
    scene::Node* drag_icon_ = nullptr;
    Listener<> drag_icon_gone_;
    wl::Connection drag_ended_;
    std::list<std::vector<wl::Connection>> virtual_pointers_;  // each one's connections

    std::vector<wl::Connection> host_input_;  // a nested host's pointer and keyboard
    std::vector<wl::Connection> connections_;
};

} // namespace atrium
