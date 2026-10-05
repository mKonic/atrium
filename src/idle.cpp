#include "idle.hpp"

#include "lock_screen.hpp"
#include "logind.hpp"
#include "output.hpp"
#include "seat.hpp"
#include "server.hpp"

#include <algorithm>

namespace atrium {

namespace {

constexpr float kDimAlpha = 0.5f;   // how dark dimmed is
constexpr int kFadeStepMs = 30;
constexpr float kFadeStep = kDimAlpha * kFadeStepMs / 1500.0f;  // over a second and a half

} // namespace

Idle::Idle(Server& server) : server_(server) {
    timer_ = wl_event_loop_add_timer(server_.loop, [](void* d) {
        static_cast<Idle*>(d)->check();
        return 0;
    }, this);
    fade_ = wl_event_loop_add_timer(server_.loop, [](void* d) {
        auto* self = static_cast<Idle*>(d);
        if (self->dim_alpha_ < kDimAlpha && (self->done_ & (1u << int(IdleStage::Dim)))) {
            self->set_dim(std::min(kDimAlpha, self->dim_alpha_ + kFadeStep));
            wl_event_source_timer_update(self->fade_, kFadeStepMs);
        }
        return 0;
    }, this);
    reconfigure();
}

Idle::~Idle() {
    if (dim_)
        wlr_scene_node_destroy(&dim_->node);
    for (wl_event_source* s : {timer_, fade_})
        if (s)
            wl_event_source_remove(s);
}

void Idle::reconfigure() {
    const Config& c = server_.config;
    const std::string* after[int(IdleStage::Count)] = {&c.dim_after, &c.screen_off_after, &c.lock_after,
                                                       &c.sleep_after};
    for (int i = 0; i < int(IdleStage::Count); ++i)
        times_.seconds[i] = idle_after_seconds(*after[i]);
    check();
}

// Cheap, as every input event comes here: the timer finds out on its own
// that the deadline moved.
void Idle::activity() {
    last_input_ = Clock::now();
    if (done_)
        undo();
}

void Idle::inhibited_changed(bool inhibited) {
    if (inhibited == inhibited_)
        return;
    inhibited_ = inhibited;
    // A video that stops starts the wait afresh, as if just watched.
    last_input_ = Clock::now();
    if (inhibited && done_)
        undo();
    check();
}

void Idle::check() {
    if (inhibited_) {
        wl_event_source_timer_update(timer_, 0);
        return;
    }
    const int64_t idle_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last_input_).count();
    IdleDue due = idle_due(times_, idle_ms, done_);
    if (due.now && server_.logind && server_.logind->idle_blocked()) {
        // systemd-inhibit --what=idle: as if there had been input.
        last_input_ = Clock::now();
        due = idle_due(times_, 0, done_);
        due.now = 0;
    }
    for (int i = 0; i < int(IdleStage::Count); ++i)
        if (due.now & (1u << i)) {
            done_ |= 1u << i;
            act(IdleStage(i));
        }
    wl_event_source_timer_update(timer_, due.next_ms < 0 ? 0 : int(std::min<int64_t>(due.next_ms + 1, 1 << 30)));
}

void Idle::act(IdleStage stage) {
    if (server_.logind && (done_ & ~(1u << int(stage))) == 0)
        server_.logind->set_idle_hint(true);  // the first stage reached
    switch (stage) {
    case IdleStage::Dim:
        wlr_log(WLR_INFO, "idle: dimming");
        if (!dim_) {
            const float black[4] = {0, 0, 0, 0};
            dim_ = wlr_scene_rect_create(server_.layer(Layer::Lock), server_.layout_box.width,
                                         server_.layout_box.height, black);
            dim_->node.data = nullptr;
            wlr_scene_node_set_position(&dim_->node, server_.layout_box.x, server_.layout_box.y);
        }
        wlr_scene_node_raise_to_top(&dim_->node);
        wl_event_source_timer_update(fade_, kFadeStepMs);
        break;
    case IdleStage::ScreenOff:
        wlr_log(WLR_INFO, "idle: screens off");
        for (Output* o : server_.outputs)
            if (o->enabled() && !o->asleep && !o->dying) {
                server_.set_screen_power(o, false);
                slept_.push_back(o);
            }
        break;
    case IdleStage::Lock:
        wlr_log(WLR_INFO, "idle: locking");
        if (server_.lock_screen)
            server_.lock_screen->lock();
        break;
    case IdleStage::Suspend:
        wlr_log(WLR_INFO, "idle: sleeping");
        if (server_.logind)
            server_.logind->suspend();
        break;
    case IdleStage::Count:
        break;
    }
}

void Idle::set_dim(float alpha) {
    dim_alpha_ = alpha;
    if (!dim_)
        return;
    const float color[4] = {0, 0, 0, alpha};  // premultiplied: black is black
    wlr_scene_rect_set_color(dim_, color);
}

// Input after idling: the screens back and bright (the lock, and the sleep,
// stay done).
void Idle::undo() {
    done_ = 0;
    wl_event_source_timer_update(fade_, 0);
    if (dim_) {
        wlr_scene_node_destroy(&dim_->node);
        dim_ = nullptr;
    }
    dim_alpha_ = 0;
    for (Output* o : slept_)
        if (std::ranges::find(server_.outputs, o) != server_.outputs.end() && o->asleep && !o->dying)
            server_.set_screen_power(o, true);
    slept_.clear();
    if (server_.logind)
        server_.logind->set_idle_hint(false);
    check();
}

} // namespace atrium
