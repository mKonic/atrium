#pragma once
#include "wl/compositor.hpp"

#include "ext-session-lock-v1-server.hpp"

namespace atrium::wl {

class Output;

// ext_session_lock_surface_v1: the lock screen on one output.
class LockSurface : public ExtSessionLockSurfaceV1, public Role {
public:
    static constexpr const char* kRole = "ext_session_lock_surface_v1";

    LockSurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, Output* output);
    ~LockSurface() override;

    const char* name() const override { return kRole; }
    bool precommit(Surface& surface) override;
    void commit(Surface& surface) override;

    Surface* surface() const { return surface_; }
    Output* output() const { return output_; }

    void* data = nullptr;  // the compositor's own object
    // Tells it the size it must be: returns the serial.
    uint32_t configure(uint32_t width, uint32_t height);

    Signal<> destroy_signal;

private:
    Surface* surface_;
    Output* output_;
    struct Sent {
        uint32_t serial, width, height;
    };
    std::vector<Sent> sent_;
    bool acked_ = false;
    uint32_t width_ = 0, height_ = 0;  // what the acked configure said
    Connection surface_gone_;
};

// ext_session_lock_v1: one client's lock of the session. The compositor
// grants it (locked(), once the screen shows nothing but it) or refuses it
// (finish()). A lock whose client goes without unlocking stays locked: that
// is the compositor's to keep.
class Lock : public ExtSessionLockV1 {
public:
    Lock(wl_client* client, uint32_t version, uint32_t id);
    ~Lock() override;

    void locked();
    void finish();
    bool was_locked() const { return locked_; }
    const std::vector<LockSurface*>& surfaces() const { return surfaces_; }

    struct {
        Signal<LockSurface*> new_surface;
        Signal<> unlock;  // unlocked properly
        Signal<> destroy;  // gone, unlocked or not
    } events;

private:
    std::vector<LockSurface*> surfaces_;
    bool locked_ = false, finished_ = false, unlocked_ = false;
};

// ext_session_lock_manager_v1.
class SessionLockManager {
public:
    explicit SessionLockManager(wl_display* display);
    ~SessionLockManager();

    Signal<Lock*> new_lock;

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

} // namespace atrium::wl
