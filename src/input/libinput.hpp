#pragma once
// Input devices straight from libinput (keyboards, pointers, touchpads,
// touchscreens, tablets, switches), opened through the session. On a
// nested or headless backend there is none: input comes from the host's
// devices or from virtual-input clients.
#include <libinput.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct wl_event_loop;
struct wl_event_source;
struct udev;

namespace atrium::input {

struct Device {
    libinput_device* handle;
    std::string name;
    bool keyboard = false, pointer = false, touch = false, tablet = false, pad = false, gesture = false,
         switches = false;
    // A touchpad: it taps and scrolls with two fingers.
    bool touchpad() const { return pointer && libinput_device_config_tap_get_finger_count(handle) > 0; }
    // The screen a touchscreen or tablet is on, when udev says (WL_OUTPUT).
    std::string output_name() const;
    void* data = nullptr;  // the seat's own
};

// A wheel, finger or knob scroll on one axis.
struct Scroll {
    enum Source : uint32_t { Wheel, Finger, Continuous, WheelTilt };
    uint32_t time_ms;
    uint32_t orientation;  // 0 vertical, 1 horizontal (wl_pointer.axis)
    double delta;          // in pointer motion units
    int32_t value120;      // wheel clicks x120; 0 for fingers and knobs
    Source source;
    bool stop;             // a finger lifted: the scroll ends (delta 0)
};

class Libinput {
public:
    // What the seat does with the events.
    struct Handler {
        virtual ~Handler() = default;
        virtual void device_added(Device& d) = 0;
        virtual void device_removed(Device& d) = 0;
        virtual void key(Device& d, uint32_t time_ms, uint32_t keycode, bool pressed) = 0;
        virtual void motion(Device& d, uint32_t time_ms, double dx, double dy, double dx_unaccel,
                            double dy_unaccel) = 0;
        virtual void motion_absolute(Device& d, uint32_t time_ms, double x, double y) = 0;  // 0..1
        virtual void button(Device& d, uint32_t time_ms, uint32_t button, bool pressed) = 0;
        virtual void scroll(Device& d, const Scroll& s) = 0;
        virtual void frame(Device& d) = 0;
        virtual void swipe(Device& d, libinput_event_gesture* e, libinput_event_type type) = 0;
        virtual void pinch(Device& d, libinput_event_gesture* e, libinput_event_type type) = 0;
        virtual void hold(Device& d, libinput_event_gesture* e, libinput_event_type type) = 0;
        virtual void touch(Device& d, libinput_event_touch* e, libinput_event_type type) = 0;
        virtual void tablet_tool(Device& d, libinput_event_tablet_tool* e, libinput_event_type type) = 0;
        virtual void tablet_pad(Device& d, libinput_event_tablet_pad* e, libinput_event_type type) = 0;
        virtual void toggle(Device& d, libinput_switch which, bool on) = 0;
    };

    // Where devices are opened: the session's seat.
    struct DeviceSeat {
        virtual ~DeviceSeat() = default;
        virtual struct udev* udev() = 0;
        virtual const char* name() = 0;
        virtual int open(const char* path) = 0;  // a fd, or -errno
        virtual void close(int fd) = 0;
    };

    // Null when it can't start (no seat, no udev).
    static std::unique_ptr<Libinput> create(DeviceSeat* seat, wl_event_loop* loop, Handler& handler);
    ~Libinput();
    Libinput(const Libinput&) = delete;
    Libinput& operator=(const Libinput&) = delete;

    const std::vector<std::unique_ptr<Device>>& devices() const { return devices_; }
    // The session went away (another VT) or came back.
    void set_active(bool active);

private:
    Libinput(DeviceSeat* seat, Handler& handler) : seat_(seat), handler_(handler) {}
    static int dispatch(int fd, uint32_t mask, void* data);
    void handle(libinput_event* e);
    Device* device_of(libinput_device* d) const;

    DeviceSeat* seat_;
    Handler& handler_;
    libinput* li_ = nullptr;
    wl_event_source* source_ = nullptr;
    std::vector<std::unique_ptr<Device>> devices_;
};

} // namespace atrium::input
