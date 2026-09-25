#include "anim.hpp"

#include "output.hpp"
#include "server.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace atrium {

namespace {

// A CSS cubic-bezier(x1, y1, x2, y2) at time t: solve x(s) = t for s, then y(s).
double bezier(double x1, double y1, double x2, double y2, double t) {
    auto at = [](double a, double b, double s) {
        return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s;
    };
    double lo = 0, hi = 1, s = t;
    for (int i = 0; i < 40; i++) {
        s = (lo + hi) / 2;
        (at(x1, x2, s) < t ? lo : hi) = s;
    }
    return at(y1, y2, s);
}

} // namespace

double ease(Ease e, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (e) {
    case Ease::Linear: return t;
    case Ease::OutCubic: return 1 - std::pow(1 - t, 3);
    case Ease::OutQuint: return 1 - std::pow(1 - t, 5);
    case Ease::InCubic: return t * t * t;
    case Ease::InOutCubic: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
    case Ease::Standard: return bezier(0.2, 0, 0, 1, t);
    case Ease::EmphasizedDecel: return bezier(0.05, 0.7, 0.1, 1, t);
    case Ease::EmphasizedAccel: return bezier(0.3, 0, 0.8, 0.15, t);
    }
    return t;
}

double Animator::now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

uint64_t Animator::start(const void* owner, double duration_ms, Ease e, Step step, Done done) {
    const Config& c = server_.config;
    const double duration = c.animations ? duration_ms / std::max(c.animation_speed, 0.05) : 0.0;
    if (duration <= 0) {
        if (step)
            step(1.0);
        if (done)
            done();
        return 0;
    }
    const uint64_t id = next_id_++;
    anims_.push_back({id, owner, now_ms(), duration, e, std::move(step), std::move(done)});
    if (anims_.back().step)
        anims_.back().step(0.0);
    request_frames();
    return id;
}

void Animator::cancel_owner(const void* owner, bool finish) {
    // Collect first: a done() may start or cancel other animations.
    std::vector<Anim> stopped;
    std::erase_if(anims_, [&](Anim& a) {
        if (a.owner != owner)
            return false;
        stopped.push_back(std::move(a));
        return true;
    });
    if (!finish)
        return;
    for (Anim& a : stopped) {
        if (a.step)
            a.step(1.0);
        if (a.done)
            a.done();
    }
}

bool Animator::tick() {
    if (anims_.empty())
        return false;
    const double now = now_ms();
    // Work on a copy: a step or done may start or cancel animations.
    const std::vector<Anim> running = anims_;
    std::vector<const Anim*> finished;
    auto alive = [this](uint64_t id) {
        return std::ranges::any_of(anims_, [id](const Anim& x) { return x.id == id; });
    };
    for (const Anim& a : running) {
        if (!alive(a.id))
            continue;  // cancelled by an earlier step this tick
        const double t = (now - a.start_ms) / a.duration_ms;
        if (a.step)
            a.step(ease(a.ease, t));
        if (t >= 1.0)
            finished.push_back(&a);
    }
    std::vector<const Anim*> completing;
    for (const Anim* f : finished)
        if (alive(f->id)) {
            std::erase_if(anims_, [&](const Anim& a) { return a.id == f->id; });
            completing.push_back(f);
        }
    for (const Anim* f : completing)
        if (f->done)
            f->done();
    if (!anims_.empty())
        request_frames();
    return !anims_.empty();
}

void Animator::request_frames() {
    for (Output* o : server_.outputs)
        if (o->enabled())
            wlr_output_schedule_frame(o->wlr);
}

} // namespace atrium
