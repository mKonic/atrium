#pragma once
#include "wl/compositor.hpp"

#include <array>
#include <string>

namespace atrium::wl {

class SeatResource;

// wl_seat: where input goes. The compositor decides focus and feeds events
// in; the seat speaks to every wl_pointer / wl_keyboard / wl_touch the focused
// client has. It also answers the requests clients make of the seat (a
// cursor image, a selection, a drag), as signals the compositor handles.
class Seat {
public:
    enum Capability : uint32_t { Pointer = 1, Keyboard = 2, Touch = 4 };

    struct Modifiers {
        uint32_t depressed = 0, latched = 0, locked = 0, group = 0;
        bool operator==(const Modifiers&) const = default;
    };

    Seat(wl_display* display, std::string name);
    ~Seat();
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    wl_display* display() const { return display_; }
    void set_capabilities(uint32_t caps);
    uint32_t capabilities() const { return caps_; }
    uint32_t next_serial();
    // Whether `serial` is one this seat sent `client` for a pointer button
    // press or touch down, recently: what a move, resize or drag must quote.
    bool validate_grab_serial(wl_client* client, uint32_t serial) const;

    // ---- keyboard ----
    // The keymap as text (xkb_keymap_get_as_string), and key repeat.
    void set_keymap(const std::string& keymap);
    void set_repeat_info(int32_t rate, int32_t delay);
    void keyboard_enter(Surface* surface, const std::vector<uint32_t>& pressed, const Modifiers& mods);
    void keyboard_clear_focus();
    void keyboard_key(uint32_t time_ms, uint32_t key, bool pressed);
    void keyboard_modifiers(const Modifiers& mods);
    Surface* keyboard_focus() const { return keyboard_focus_; }

    // ---- pointer ----
    void pointer_enter(Surface* surface, double sx, double sy);
    void pointer_clear_focus();
    void pointer_motion(uint32_t time_ms, double sx, double sy);
    uint32_t pointer_button(uint32_t time_ms, uint32_t button, bool pressed);
    enum class AxisSource : uint32_t { Wheel, Finger, Continuous, WheelTilt };
    void pointer_axis(uint32_t time_ms, uint32_t orientation, double value, int32_t value120, AxisSource source,
                      bool inverted);
    void pointer_frame();
    Surface* pointer_focus() const { return pointer_focus_; }
    double pointer_x() const { return pointer_x_; }
    double pointer_y() const { return pointer_y_; }
    // Buttons held on the focused surface.
    size_t pointer_buttons() const { return buttons_.size(); }

    // ---- touch ----
    uint32_t touch_down(uint32_t time_ms, Surface* surface, int32_t id, double sx, double sy);
    void touch_up(uint32_t time_ms, int32_t id);
    void touch_motion(uint32_t time_ms, int32_t id, double sx, double sy);
    void touch_frame();
    void touch_cancel();

    // A client asked for a pointer image: `surface` null hides the pointer.
    struct CursorRequest {
        wl_client* client;
        Surface* surface;
        int32_t hotspot_x, hotspot_y;
    };

    struct {
        Signal<const CursorRequest&> request_cursor;
        Signal<Surface*> keyboard_focus;  // after the enter (or clear)
        Signal<Surface*> pointer_focus;
        Signal<wl_client*> keyboard_client;  // the focused client changed (null: none)
    } events;

    // The seat a wl_seat names.
    static Seat* from(wl_resource* resource);
    static Seat* from(WlSeat* resource);
    std::vector<SeatResource*> resources_for(wl_client* client) const;
    // Whether a wl_pointer / wl_keyboard came from this seat.
    bool has_pointer(wl_resource* pointer) const;
    bool has_keyboard(wl_resource* keyboard) const;
    // The keyboard-focused client's wl_keyboard objects.
    std::vector<WlKeyboard*> keyboards_for(wl_client* client) const;

private:
    friend class SeatResource;
    void bind_keyboard(WlKeyboard* keyboard);
    void send_keymap(WlKeyboard* keyboard);
    template <class Fn>
    void each_pointer(wl_client* client, Fn fn);
    template <class Fn>
    void each_keyboard(wl_client* client, Fn fn);
    template <class Fn>
    void each_touch(wl_client* client, Fn fn);
    void remember_serial(wl_client* client, uint32_t serial);

    wl_display* display_;
    std::string name_;
    uint32_t caps_ = 0;
    std::unique_ptr<Global> global_;
    std::vector<Weak<SeatResource>> resources_;

    std::string keymap_;
    int32_t repeat_rate_ = 25, repeat_delay_ = 600;
    Surface* keyboard_focus_ = nullptr;
    Signal<>::Connection keyboard_focus_gone_;
    Modifiers mods_;
    std::vector<uint32_t> keys_;

    Surface* pointer_focus_ = nullptr;
    Signal<>::Connection pointer_focus_gone_;
    double pointer_x_ = 0, pointer_y_ = 0;
    std::vector<uint32_t> buttons_;
    uint32_t pointer_enter_serial_ = 0;
    bool pointer_frame_pending_ = false;

    struct TouchPoint {
        int32_t id;
        Surface* surface;
        Signal<>::Connection gone;
    };
    std::vector<std::unique_ptr<TouchPoint>> touches_;

    // Grab serials recently sent, per client (a small ring).
    struct Serial {
        wl_client* client;
        uint32_t serial;
    };
    std::array<Serial, 32> serials_{};
    size_t next_serial_slot_ = 0;
};

class SeatResource : public WlSeat {
public:
    SeatResource(wl_client* client, uint32_t version, uint32_t id, Seat* seat);
    Seat* seat;  // null once the seat is gone
    std::vector<Weak<WlPointer>> pointers;
    std::vector<Weak<WlKeyboard>> keyboards;
    std::vector<Weak<WlTouch>> touches;
};

} // namespace atrium::wl
