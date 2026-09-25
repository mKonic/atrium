#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace atrium {

class Server;

// The last three are caelestia's curves (Material 3's): standard
// (0.2, 0, 0, 1), emphasized decelerate (0.05, 0.7, 0.1, 1) and emphasized
// accelerate (0.3, 0, 0.8, 0.15).
enum class Ease { Linear, OutCubic, OutQuint, InCubic, InOutCubic, Standard, EmphasizedDecel, EmphasizedAccel };

double ease(Ease e, double t);

// Time-based animations advanced from the output frame loop. Each animation
// calls `step` with its eased progress (0..1) and `done` once it reaches 1.
// Everything an animation touches must outlive it or be registered as its
// `owner`: cancel_owner() drops them all before that owner goes away.
class Animator {
public:
    explicit Animator(Server& server) : server_(server) {}

    using Step = std::function<void(double)>;
    using Done = std::function<void()>;

    // Duration in milliseconds at speed 1; scaled by the animation settings.
    // With animations off, step(1) and done() run immediately.
    uint64_t start(const void* owner, double duration_ms, Ease e, Step step, Done done = {});

    // Stop every animation of `owner`. `finish` runs step(1) and done();
    // otherwise they are dropped as they are.
    void cancel_owner(const void* owner, bool finish);

    // Advance to now. Returns whether anything is still running.
    bool tick();

    bool active() const { return !anims_.empty(); }

private:
    struct Anim {
        uint64_t id;
        const void* owner;
        double start_ms, duration_ms;
        Ease ease;
        Step step;
        Done done;
    };

    void request_frames();
    static double now_ms();

    Server& server_;
    std::vector<Anim> anims_;
    uint64_t next_id_ = 1;
};

} // namespace atrium
