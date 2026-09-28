#pragma once
#include "wl/seat.hpp"

#include <chrono>
#include <map>

namespace atrium::wl {

// zwp_relative_pointer_manager_v1: raw motion for games and 3D apps, sent
// alongside wl_pointer motion to the focused client.
class RelativePointers {
public:
    RelativePointers(wl_display* display, Seat& seat);
    ~RelativePointers();
    void send_motion(uint64_t time_us, double dx, double dy, double dx_unaccel, double dy_unaccel);

private:
    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_, pointers_;
};

// zwp_pointer_constraints_v1: a surface locks the pointer in place (a game's
// mouselook) or confines it to a region. The compositor decides when a
// constraint is in force (its surface has focus) and tells the client.
class PointerConstraints {
public:
    enum class Type { Lock, Confine };
    struct Constraint {
        Type type;
        Surface* surface;
        Region region;  // applied with the surface's commit; infinite: the whole surface
        bool persistent;  // stays after being turned off (else it is spent)
        bool active = false;
        bool spent = false;
        std::optional<std::pair<double, double>> cursor_hint;  // lock: where the app draws its cursor
        Weak<Resource> resource;
        Region pending_region;
        bool region_pending = false;
        std::optional<std::pair<double, double>> pending_hint;
        Connection commit, surface_gone;
    };

    PointerConstraints(wl_display* display, Seat& seat);
    ~PointerConstraints();

    Constraint* for_surface(Surface* surface) const;
    void activate(Constraint* c);
    void deactivate(Constraint* c);

    struct {
        Signal<Constraint*> new_constraint;
        Signal<Constraint*> region_changed;
        Signal<Constraint*> destroy;
    } events;

private:
    void drop(Constraint* c);

    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Constraint>> constraints_;
};

// zwp_pointer_gestures_v1: touchpad swipes, pinches and holds, to the
// client under the pointer (a browser's pinch zoom).
class PointerGestures {
public:
    PointerGestures(wl_display* display, Seat& seat);
    ~PointerGestures();

    void swipe_begin(uint32_t time_ms, uint32_t fingers);
    void swipe_update(uint32_t time_ms, double dx, double dy);
    void swipe_end(uint32_t time_ms, bool cancelled);
    void pinch_begin(uint32_t time_ms, uint32_t fingers);
    void pinch_update(uint32_t time_ms, double dx, double dy, double scale, double rotation);
    void pinch_end(uint32_t time_ms, bool cancelled);
    void hold_begin(uint32_t time_ms, uint32_t fingers);
    void hold_end(uint32_t time_ms, bool cancelled);

private:
    template <class T, class Fn>
    void each(std::vector<Weak<Resource>>& list, Fn fn);

    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_, swipes_, pinches_, holds_;
    Surface* gesture_surface_ = nullptr;  // where the gesture began
    Connection gesture_gone_;
};

// zwp_keyboard_shortcuts_inhibit_manager_v1: a remote desktop or VM viewer
// asks for every key, atrium's shortcuts included.
class ShortcutInhibitors {
public:
    struct Inhibitor {
        Surface* surface;
        Seat* seat;
        bool active = false;
        Weak<Resource> resource;
        Connection surface_gone;
    };
    ShortcutInhibitors(wl_display* display);
    ~ShortcutInhibitors();

    Inhibitor* for_surface(Surface* surface) const;
    void set_active(Inhibitor* inhibitor, bool active);
    Signal<Inhibitor*> new_inhibitor, destroy;

private:
    void drop(Inhibitor* i);
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Inhibitor>> inhibitors_;
};

// wp_cursor_shape_manager_v1: a client names a cursor ("text", "pointer")
// instead of drawing one.
class CursorShapes {
public:
    CursorShapes(wl_display* display, Seat& seat);
    ~CursorShapes();

    struct Request {
        wl_client* client;
        uint32_t serial;
        uint32_t shape;  // wp_cursor_shape_device_v1.shape
        bool tablet_tool;
    };
    Signal<const Request&> request_shape;
    // The xcursor name of a shape ("default", "text"...).
    static const char* name_of(uint32_t shape);

private:
    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_, devices_;
};

// zwp_idle_inhibit_manager_v1: a video player keeps the screen awake while
// its surface is visible.
class IdleInhibitors {
public:
    explicit IdleInhibitors(wl_display* display);
    ~IdleInhibitors();

    // The surfaces asking now (the compositor checks they are visible).
    std::vector<Surface*> surfaces() const;
    Signal<> changed;

private:
    struct Inhibitor {
        Surface* surface;
        Weak<Resource> resource;
        Connection surface_gone;
    };
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Inhibitor>> inhibitors_;
};

// ext_idle_notifier_v1: tells clients (hypridle, swayidle) when the user
// has been idle for their timeout, and when they're back.
class IdleNotifier {
public:
    IdleNotifier(wl_display* display, Seat& seat);
    ~IdleNotifier();

    // Input happened: every notification restarts.
    void activity();
    // Something (a playing video) holds idle off; input-idle
    // notifications (v2) don't care.
    void set_inhibited(bool inhibited);

private:
    struct Notification;
    void arm(Notification* n);

    wl_display* display_;
    Seat& seat_;
    bool inhibited_ = false;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::vector<std::unique_ptr<Notification>> notifications_;
};

// wp_pointer_warp_v1: a client moves the pointer within its own surface.
class PointerWarps {
public:
    PointerWarps(wl_display* display, Seat& seat);
    ~PointerWarps();

    struct Request {
        Surface* surface;
        double x, y;
        uint32_t serial;
    };
    Signal<const Request&> request_warp;

private:
    Seat& seat_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

} // namespace atrium::wl
