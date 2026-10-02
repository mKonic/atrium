#pragma once
// Where outputs (and, nested, input devices) come from: a DRM device, a
// window on a host compositor, or nothing at all (headless).
#include "backend/output.hpp"

#include <memory>
#include <vector>

struct wl_event_loop;

namespace atrium::backend {

class Backend {
public:
    virtual ~Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    virtual bool start() = 0;
    // The DRM render node or device the renderer should use (-1: any).
    virtual int drm_fd() const { return -1; }
    // What buffers its outputs take (BUFFER_CAP_*).
    virtual uint32_t buffer_caps() const = 0;
    // Several outputs' states at once, all or none (a layout change).
    virtual bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only);
    // A virtual screen (headless or nested); null where there are none.
    virtual Output* create_output() { return nullptr; }
    // Whether an output belongs to a virtual screen it made.
    virtual bool is_virtual(const Output*) const { return false; }
    // Removes a virtual screen it made (events.destroy, then gone).
    virtual bool destroy_output(Output*) { return false; }
    // Whether its outputs take wait and signal timelines (explicit sync).
    virtual bool supports_timelines() const { return false; }
    // Whether it is a real display (DRM).
    virtual bool is_drm() const { return false; }

    wl_event_loop* loop() const { return loop_; }

    // A nested host's pointer and keyboard, over its windows.
    struct HostAxis {
        uint32_t time;
        uint32_t orientation;  // wl_pointer.axis
        double delta;
        int32_t value120;  // 0: not a wheel step
        uint32_t source;   // wl_pointer.axis_source
        bool inverted;     // natural scrolling
    };

    struct {
        wl::Signal<Output*> new_output;
        // The pointer over `output` at a fraction (0..1) of it.
        wl::Signal<Output*, uint32_t /*time*/, double /*fx*/, double /*fy*/> host_motion;
        wl::Signal<uint32_t /*time*/, uint32_t /*button*/, bool /*pressed*/> host_button;
        wl::Signal<const HostAxis&> host_axis;
        wl::Signal<> host_frame;
        wl::Signal<uint32_t /*time*/, uint32_t /*key*/, bool /*pressed*/> host_key;
        // The host session ended: nothing more will come.
        wl::Signal<> gone;
        wl::Signal<> destroy;
    } events;

protected:
    explicit Backend(wl_event_loop* loop) : loop_(loop) {}

private:
    wl_event_loop* loop_;
};

// Several backends as one (DRM and the virtual screens added later).
class Multi final : public Backend {
public:
    explicit Multi(wl_event_loop* loop) : Backend(loop) {}
    ~Multi() override;

    void add(std::unique_ptr<Backend> b);
    // Destroys `b` (its outputs go first).
    void remove(Backend* b);
    bool start() override;
    int drm_fd() const override;
    uint32_t buffer_caps() const override;
    bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) override;
    Output* create_output() override;
    bool is_virtual(const Output* o) const override;
    bool destroy_output(Output* o) override;
    bool is_drm() const override;
    bool supports_timelines() const override;
    const std::vector<std::unique_ptr<Backend>>& backends() const { return backends_; }

private:
    std::vector<std::unique_ptr<Backend>> backends_;
    std::vector<wl::Connection> connections_;
    bool started_ = false;
};

} // namespace atrium::backend
