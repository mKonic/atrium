#pragma once
// Where outputs (and, nested, input devices) come from: a DRM device, a
// window on a host compositor, or nothing at all (headless).
#include "backend/output.hpp"

#include <memory>
#include <vector>

struct wl_event_loop;
struct wlr_input_device;

namespace atrium::backend {

class Backend {
public:
    virtual ~Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    virtual bool start() = 0;
    // The DRM render node or device the renderer should use (-1: any).
    virtual int drm_fd() const { return -1; }
    // What buffers its outputs take (WLR_BUFFER_CAP_*).
    virtual uint32_t buffer_caps() const = 0;
    // Several outputs' states at once, all or none (a layout change).
    virtual bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only);
    // A virtual screen (headless or nested); null where there are none.
    virtual Output* create_output() { return nullptr; }
    // Whether an output belongs to a virtual screen it made.
    virtual bool is_virtual(const Output*) const { return false; }
    // Whether it is a real display (DRM).
    virtual bool is_drm() const { return false; }

    wl_event_loop* loop() const { return loop_; }

    struct {
        wl::Signal<Output*> new_output;
        wl::Signal<wlr_input_device*> new_input;  // a nested host's keyboard and pointer
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
    bool start() override;
    int drm_fd() const override;
    uint32_t buffer_caps() const override;
    bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) override;
    Output* create_output() override;
    bool is_virtual(const Output* o) const override;
    bool is_drm() const override;
    const std::vector<std::unique_ptr<Backend>>& backends() const { return backends_; }

private:
    std::vector<std::unique_ptr<Backend>> backends_;
    std::vector<wl::Connection> connections_;
    bool started_ = false;
};

} // namespace atrium::backend
