#include "anim.hpp"

#include "output.hpp"
#include "server.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace atrium {

double ease(Ease e, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (e) {
    case Ease::Linear: return t;
    case Ease::OutCubic: return 1 - std::pow(1 - t, 3);
    case Ease::OutQuint: return 1 - std::pow(1 - t, 5);
    case Ease::InCubic: return t * t * t;
    case Ease::InOutCubic: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
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
