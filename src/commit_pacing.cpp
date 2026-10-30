#include "commit_pacing.hpp"

#include "output.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wlr.hpp"

#include "commit-timing-v1-protocol.h"
#include "fifo-v1-protocol.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace atrium {

namespace {

// A held commit may not wait longer than this, whatever happens to the
// screens (the protocol asks for a finite wait).
constexpr int64_t kLongestHoldMs = 1000;

int64_t now_ns() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

} // namespace

struct CommitPacing::Surface {
    CommitPacing* owner;
    wlr_surface* s;
    wl_resource* fifo = nullptr;
    wl_resource* timer = nullptr;

    // What the next commit asks for.
    bool set_barrier = false, wait_barrier = false;
    bool timed = false;
    int64_t target_ns = 0;

    // A commit made with set_barrier (state barrier_state), not yet shown.
    // barrier_after: each screen's commit count when that state was applied;
    // empty until it is. The barrier clears once a screen presents a later
    // commit, which drew it: clearing it any sooner lets the next commit
    // replace it unseen, and its presentation feedback is discarded.
    bool barrier = false;
    uint32_t barrier_state = 0;
    std::vector<std::pair<wlr_output*, uint32_t>> barrier_after;

    struct Wake {
        Surface* self;
        uint32_t seq;
    };
    struct Held {
        uint32_t seq;
        bool fifo_lock = false, timer_lock = false;
        bool sets_barrier = false;
        int64_t target_ns = 0;
        int64_t since_ns = 0;
        wl_event_source* timer = nullptr;
        Wake* wake = nullptr;  // the timer's own data
    };
    std::deque<Held> held;

    wl_listener commit{}, applied_listener{}, destroy{};

    bool on(wlr_output* o) const {
        wlr_surface_output* so;
        wl_list_for_each(so, &s->current_outputs, link)
            if (so->output == o)
                return true;
        return false;
    }

    // Shown somewhere that doesn't tear: holding it back makes sense there.
    bool visible() const {
        bool any = false;
        wlr_surface_output* so;
        wl_list_for_each(so, &s->current_outputs, link) {
            if (owner->tearing(so->output))
                return false;
            any = true;
        }
        return any;
    }

    void release(Held& h, bool fifo_lock, bool timer_lock) {
        if (fifo_lock && h.fifo_lock) {
            h.fifo_lock = false;
            wlr_surface_unlock_cached(s, h.seq);
        }
        if (timer_lock && h.timer_lock) {
            h.timer_lock = false;
            stop_timer(h);
            wlr_surface_unlock_cached(s, h.seq);
        }
        if (!h.fifo_lock && !h.timer_lock && h.sets_barrier) {
            h.sets_barrier = false;
            raise_barrier(h.seq);
        }
    }

    void raise_barrier(uint32_t state) {
        barrier = true;
        barrier_state = state;
        barrier_after.clear();
        applied();
    }

    // The surface's state changed: note when the barrier's state got applied.
    void applied() {
        if (!barrier || !barrier_after.empty() || int32_t(s->current.seq - barrier_state) < 0)
            return;
        wlr_surface_output* so;
        wl_list_for_each(so, &s->current_outputs, link) {
            barrier_after.emplace_back(so->output, so->output->commit_seq);
            owner->watch(so->output);
        }
    }

    static void stop_timer(Held& h) {
        if (h.timer)
            wl_event_source_remove(h.timer);
        delete h.wake;
        h.timer = nullptr;
        h.wake = nullptr;
    }

    void prune() {
        std::erase_if(held, [](const Held& h) { return !h.fifo_lock && !h.timer_lock; });
    }

    // No barrier up: commits waiting on one go through in order, up to one
    // that sets the next (once it's gone, or while its time still holds it).
    // A frame's commit and the empty one NVIDIA's driver sends after it
    // both queue behind the frame before, and only the first sets a barrier.
    void drain() {
        for (Held& h : held) {
            if (barrier)
                break;
            if (h.fifo_lock)
                release(h, true, false);
            if (h.sets_barrier)
                break;
        }
        prune();
    }

    void release_all() {
        for (Held& h : held)
            release(h, true, true);
        held.clear();
        barrier = false;
        barrier_after.clear();
    }

    void client_commit() {
        const bool set = std::exchange(set_barrier, false);
        const bool wait = std::exchange(wait_barrier, false);
        const bool has_time = std::exchange(timed, false);
        const int64_t target = target_ns;

        // A synchronized subsurface goes with its parent (fifo-v1).
        wlr_subsurface* sub = wlr_subsurface_try_from_wlr_surface(s);
        const bool synced = sub && sub->synchronized;
        const bool show = visible();
        const bool fifo_hold =
            wait && !synced && show && (barrier || std::ranges::any_of(held, [](const Held& h) { return h.fifo_lock || h.sets_barrier; }));
        const int64_t now = now_ns();
        // A time beyond the longest hold is not a frame time (NVIDIA's Vulkan
        // driver, its schedule run away, asked for hours ahead): holding it
        // would only make every frame wait out the safety net.
        const bool time_hold = has_time && show && target > now && target - now <= kLongestHoldMs * 1'000'000;

        if (!fifo_hold && !time_hold) {
            // Applied right after this, unless another lock holds it.
            if (set)
                raise_barrier(s->pending.seq);
            return;
        }
        Held h;
        h.since_ns = now;
        h.sets_barrier = set;
        if (fifo_hold) {
            h.seq = wlr_surface_lock_pending(s);
            h.fifo_lock = true;
            // A frame must come to let it go, damage or not.
            wlr_surface_output* so;
            wl_list_for_each(so, &s->current_outputs, link) {
                owner->watch(so->output);
                wlr_output_schedule_frame(so->output);
            }
        }
        if (time_hold) {
            h.seq = wlr_surface_lock_pending(s);
            h.timer_lock = true;
            h.target_ns = target;
            // Ends at the time at the latest; a refresh just before it lets it go sooner.
            h.wake = new Wake{this, h.seq};
            h.timer = wl_event_loop_add_timer(owner->server.loop, [](void* data) {
                const Wake w = *static_cast<Wake*>(data);
                for (Held& x : w.self->held)
                    if (x.seq == w.seq && x.timer_lock)
                        w.self->release(x, false, true);
                w.self->drain();
                return 0;
            }, h.wake);
            wl_event_source_timer_update(h.timer, int(std::max<int64_t>(1, (target - now + 999'999) / 1'000'000)));
            wlr_surface_output* so;
            wl_list_for_each(so, &s->current_outputs, link)
                owner->watch(so->output);
        }
        held.push_back(h);
    }
};

struct CommitPacing::Screen {
    CommitPacing* owner;
    wlr_output* output;
    wl_listener frame{}, present{}, destroy{};
};

namespace {

using Surface = CommitPacing::Surface;

Surface* fifo_surface(wl_resource* r) {
    return static_cast<Surface*>(wl_resource_get_user_data(r));
}

const struct wp_fifo_v1_interface fifo_impl = {
    .set_barrier =
        [](wl_client*, wl_resource* r) {
            if (Surface* s = fifo_surface(r))
                s->set_barrier = true;
            else
                wl_resource_post_error(r, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the surface is gone");
        },
    .wait_barrier =
        [](wl_client*, wl_resource* r) {
            if (Surface* s = fifo_surface(r))
                s->wait_barrier = true;
            else
                wl_resource_post_error(r, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the surface is gone");
        },
    .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

const struct wp_commit_timer_v1_interface timer_impl = {
    .set_timestamp =
        [](wl_client*, wl_resource* r, uint32_t hi, uint32_t lo, uint32_t nsec) {
            Surface* s = fifo_surface(r);
            if (!s) {
                wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED, "the surface is gone");
                return;
            }
            if (nsec >= 1'000'000'000) {
                wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP, "tv_nsec out of range");
                return;
            }
            if (s->timed) {
                wl_resource_post_error(r, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS, "a timestamp is already set");
                return;
            }
            s->timed = true;
            s->target_ns = int64_t((uint64_t(hi) << 32) | lo) * 1'000'000'000 + nsec;
        },
    .destroy = [](wl_client*, wl_resource* r) { wl_resource_destroy(r); },
};

void unlink_resource(wl_resource* r) {
    if (Surface* s = fifo_surface(r)) {
        if (s->fifo == r)
            s->fifo = nullptr;
        if (s->timer == r)
            s->timer = nullptr;
    }
}

} // namespace

CommitPacing::CommitPacing(Server& srv) : server(srv) {
    fifo_global_ = wl_global_create(server.display, &wp_fifo_manager_v1_interface, 1, this,
        [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            wl_resource* m = wl_resource_create(client, &wp_fifo_manager_v1_interface, int(version), id);
            if (!m) {
                wl_client_post_no_memory(client);
                return;
            }
            static const struct wp_fifo_manager_v1_interface impl = {
                .destroy = [](wl_client*, wl_resource* m) { wl_resource_destroy(m); },
                .get_fifo =
                    [](wl_client* client, wl_resource* m, uint32_t id, wl_resource* surface) {
                        auto* self = static_cast<CommitPacing*>(wl_resource_get_user_data(m));
                        Surface* s = self->surface_for(wlr_surface_from_resource(surface), true);
                        if (s->fifo) {
                            wl_resource_post_error(m, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS, "the surface has a fifo");
                            return;
                        }
                        wl_resource* r = wl_resource_create(client, &wp_fifo_v1_interface, wl_resource_get_version(m), id);
                        if (!r) {
                            wl_client_post_no_memory(client);
                            return;
                        }
                        s->fifo = r;
                        wl_resource_set_implementation(r, &fifo_impl, s, unlink_resource);
                    },
            };
            wl_resource_set_implementation(m, &impl, data, nullptr);
        });
    timing_global_ = wl_global_create(server.display, &wp_commit_timing_manager_v1_interface, 1, this,
        [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            wl_resource* m = wl_resource_create(client, &wp_commit_timing_manager_v1_interface, int(version), id);
            if (!m) {
                wl_client_post_no_memory(client);
                return;
            }
            static const struct wp_commit_timing_manager_v1_interface impl = {
                .destroy = [](wl_client*, wl_resource* m) { wl_resource_destroy(m); },
                .get_timer =
                    [](wl_client* client, wl_resource* m, uint32_t id, wl_resource* surface) {
                        auto* self = static_cast<CommitPacing*>(wl_resource_get_user_data(m));
                        Surface* s = self->surface_for(wlr_surface_from_resource(surface), true);
                        if (s->timer) {
                            wl_resource_post_error(m, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
                                                   "the surface has a commit timer");
                            return;
                        }
                        wl_resource* r =
                            wl_resource_create(client, &wp_commit_timer_v1_interface, wl_resource_get_version(m), id);
                        if (!r) {
                            wl_client_post_no_memory(client);
                            return;
                        }
                        s->timer = r;
                        wl_resource_set_implementation(r, &timer_impl, s, unlink_resource);
                    },
            };
            wl_resource_set_implementation(m, &impl, data, nullptr);
        });
    // The last word on "finite": anything held too long goes through.
    safety_ = wl_event_loop_add_timer(server.loop, [](void* data) {
        auto* self = static_cast<CommitPacing*>(data);
        const int64_t now = now_ns();
        for (auto& [_, s] : self->surfaces_) {
            for (Surface::Held& h : s->held)
                if (now - h.since_ns > kLongestHoldMs * 1'000'000)
                    s->release(h, true, true);
            s->drain();
        }
        wl_event_source_timer_update(self->safety_, 250);
        return 0;
    }, this);
    wl_event_source_timer_update(safety_, 250);
}

CommitPacing::~CommitPacing() {
    if (safety_)
        wl_event_source_remove(safety_);
    for (auto& [_, screen] : screens_) {
        wl_list_remove(&screen->frame.link);
        wl_list_remove(&screen->present.link);
        wl_list_remove(&screen->destroy.link);
    }
    for (auto& [_, s] : std::unordered_map<wlr_surface*, std::unique_ptr<Surface>>(std::move(surfaces_))) {
        s->release_all();
        if (s->fifo)
            wl_resource_set_user_data(s->fifo, nullptr);
        if (s->timer)
            wl_resource_set_user_data(s->timer, nullptr);
        wl_list_remove(&s->commit.link);
        wl_list_remove(&s->applied_listener.link);
        wl_list_remove(&s->destroy.link);
    }
    if (fifo_global_)
        wl_global_destroy(fifo_global_);
    if (timing_global_)
        wl_global_destroy(timing_global_);
}

CommitPacing::Surface* CommitPacing::surface_for(wlr_surface* s, bool create) {
    if (auto it = surfaces_.find(s); it != surfaces_.end())
        return it->second.get();
    if (!create)
        return nullptr;
    auto p = std::make_unique<Surface>();
    Surface* sp = p.get();
    sp->owner = this;
    sp->s = s;
    sp->commit.notify = [](wl_listener* l, void*) {
        Surface* self = wl_container_of(l, self, commit);
        self->client_commit();
    };
    wl_signal_add(&s->events.client_commit, &sp->commit);
    sp->applied_listener.notify = [](wl_listener* l, void*) {
        Surface* self = wl_container_of(l, self, applied_listener);
        self->applied();
    };
    wl_signal_add(&s->events.commit, &sp->applied_listener);
    sp->destroy.notify = [](wl_listener* l, void*) {
        Surface* self = wl_container_of(l, self, destroy);
        self->owner->drop(self);
    };
    wl_signal_add(&s->events.destroy, &sp->destroy);
    surfaces_.emplace(s, std::move(p));
    return sp;
}

void CommitPacing::drop(Surface* s) {
    // The surface is going: its cached states go with it, unlocked or not.
    for (Surface::Held& h : s->held)
        Surface::stop_timer(h);
    s->held.clear();
    if (s->fifo)
        wl_resource_set_user_data(s->fifo, nullptr);
    if (s->timer)
        wl_resource_set_user_data(s->timer, nullptr);
    wl_list_remove(&s->commit.link);
    wl_list_remove(&s->applied_listener.link);
    wl_list_remove(&s->destroy.link);
    surfaces_.erase(s->s);
}

void CommitPacing::watch(wlr_output* output) {
    if (screens_.contains(output))
        return;
    auto sc = std::make_unique<Screen>();
    sc->owner = this;
    sc->output = output;
    sc->frame.notify = [](wl_listener* l, void*) {
        Screen* self = wl_container_of(l, self, frame);
        self->owner->refreshed(self->output);
    };
    wl_signal_add(&output->events.frame, &sc->frame);
    sc->present.notify = [](wl_listener* l, void* data) {
        Screen* self = wl_container_of(l, self, present);
        self->owner->presented(self->output, static_cast<wlr_output_event_present*>(data)->commit_seq);
    };
    wl_signal_add(&output->events.present, &sc->present);
    sc->destroy.notify = [](wl_listener* l, void*) {
        Screen* self = wl_container_of(l, self, destroy);
        CommitPacing* owner = self->owner;
        wl_list_remove(&self->frame.link);
        wl_list_remove(&self->present.link);
        wl_list_remove(&self->destroy.link);
        owner->screens_.erase(self->output);
    };
    wl_signal_add(&output->events.destroy, &sc->destroy);
    screens_.emplace(output, std::move(sc));
}

bool CommitPacing::tearing(wlr_output* output) const {
    auto* out = static_cast<Output*>(output->data);
    return out && tearing_view(server, *out) != nullptr;
}

void CommitPacing::refreshed(wlr_output* output) {
    const int64_t now = now_ns();
    const int64_t period = output->refresh > 0 ? int64_t(1e12 / output->refresh) : 16'666'667;
    for (auto& [_, s] : surfaces_) {
        if (s->held.empty() && !s->barrier)
            continue;
        // Nowhere to be seen any more: nothing waits for it.
        if (wl_list_empty(&s->s->current_outputs)) {
            s->release_all();
            continue;
        }
        if (!s->on(output))
            continue;
        // Commits timed for this refresh or before it go through now.
        for (Surface::Held& h : s->held)
            if (h.timer_lock && h.target_ns <= now + period)
                s->release(h, false, true);
        s->drain();
        // Waiting on a barrier: a frame must come to show what set it.
        if (s->barrier && std::ranges::any_of(s->held, [](const Surface::Held& h) { return h.fifo_lock; }))
            wlr_output_schedule_frame(output);
    }
}

void CommitPacing::presented(wlr_output* output, uint32_t commit_seq) {
    for (auto& [_, s] : surfaces_) {
        if (!s->barrier)
            continue;
        const auto it = std::ranges::find(s->barrier_after, output, &std::pair<wlr_output*, uint32_t>::first);
        if (it == s->barrier_after.end() || int32_t(commit_seq - it->second) <= 0)
            continue;
        // What set the barrier is on the screen: it clears, and what waited
        // on it goes through (it may set the next).
        s->barrier = false;
        s->barrier_after.clear();
        s->drain();
    }
}

} // namespace atrium
