#include "wl/timing.hpp"

#include "wl/dmabuf.hpp"
#include "wl/output.hpp"

#include "commit-timing-v1-server.hpp"
#include "fifo-v1-server.hpp"
#include "linux-drm-syncobj-v1-server.hpp"
#include "presentation-time-server.hpp"

extern "C" {
#include <wlr/render/drm_syncobj.h>
}
#include <xf86drm.h>

#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace atrium::wl {

namespace {

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// One feedback: unless it is presented, it says discarded when dropped.
struct Feedback {
    Weak<WpPresentationFeedback> resource;
    bool done = false;
    ~Feedback() {
        if (!done)
            if (WpPresentationFeedback* r = resource.get()) {
                r->send_discarded();
                r->destroy();
            }
    }
};

} // namespace

// ---- presentation -----------------------------------------------------------------

Presentation::Presentation(wl_display* display) {
    global_ = Global::create<WpPresentation>(display, 2, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<WpPresentation>(client, version, id);
        if (!m)
            return;
        m->on_feedback([](WpPresentation* self, wl_resource* surface_res, uint32_t id) {
            auto* f = make<WpPresentationFeedback>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!f)
                return;
            if (!s) {
                f->send_discarded();
                f->destroy();
                return;
            }
            auto fb = std::make_shared<Feedback>();
            fb->resource = f;
            s->pending_state().feedbacks.push_back(std::move(fb));
            s->pending_state().committed |= SurfaceState::Presentation;
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
        m->send_clock_id(CLOCK_MONOTONIC);
    });
}

Presentation::~Presentation() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
}

void Presentation::presented(Surface* surface, Output* output, const timespec& when, uint32_t refresh_ns,
                             uint64_t seq, uint32_t flags) {
    for (auto& p : surface->take_feedbacks()) {
        auto fb = std::static_pointer_cast<Feedback>(p);
        WpPresentationFeedback* r = fb->resource.get();
        if (!r)
            continue;
        fb->done = true;
        if (output)
            for (WlOutput* o : output->resources_for(r->client()))
                r->send_sync_output(o->resource());
        const uint64_t sec = uint64_t(when.tv_sec);
        r->send_presented(uint32_t(sec >> 32), uint32_t(sec & 0xffffffff), uint32_t(when.tv_nsec), refresh_ns,
                          uint32_t(seq >> 32), uint32_t(seq & 0xffffffff), flags);
        r->destroy();
    }
}

// ---- fifo ---------------------------------------------------------------------------

struct Fifo::State {
    Weak<WpFifoV1> resource;
    bool barrier = false;  // set by an applied commit, until the next refresh
    Connection queued, committed, gone;
};

Fifo::Fifo(wl_display* display) {
    global_ = Global::create<WpFifoManagerV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<WpFifoManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_fifo([this](WpFifoManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* f = make<WpFifoV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!f)
                return;
            if (!s) {
                f->detach();
                return;
            }
            if (surfaces_.contains(s)) {
                self->post_error(uint32_t(WpFifoManagerV1::Error::AlreadyExists), "the surface already has one");
                return;
            }
            auto st = std::make_unique<State>();
            State* sp = st.get();
            sp->resource = f;
            surfaces_[s] = std::move(st);
            f->on_set_barrier([s](WpFifoV1*) {
                s->pending_state().fifo_barrier = true;
                s->pending_state().committed |= SurfaceState::Fifo;
            });
            f->on_wait_barrier([s](WpFifoV1*) {
                s->pending_state().fifo_wait = true;
                s->pending_state().committed |= SurfaceState::Fifo;
            });
            // A waiting commit holds while a barrier is up, or one ahead of it
            // in the queue will put one up.
            sp->queued = s->events.queued.connect([s, sp](SurfaceState* st) {
                if (!st->fifo_wait)
                    return;
                bool ahead = false;
                for (const auto& q : s->queued_states()) {
                    if (q.get() == st)
                        break;
                    ahead |= q->fifo_barrier;
                }
                if (sp->barrier || ahead)
                    st->locks |= SurfaceState::LockFifo;
            });
            sp->committed = s->events.commit.connect([s, sp] {
                if (s->current().fifo_barrier)
                    sp->barrier = true;
            });
            auto drop = [this, s] {
                auto it = surfaces_.find(s);
                if (it == surfaces_.end())
                    return;
                if (WpFifoV1* r = it->second->resource.get())
                    r->detach();
                auto owned = std::move(it->second);
                surfaces_.erase(it);
                // Nothing may stay held by a fifo that's gone.
                for (const auto& q : s->queued_states())
                    q->locks &= ~SurfaceState::LockFifo;
                (void)owned;
            };
            sp->gone = s->events.destroy.connect(drop);
            f->on_gone(drop);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Fifo::~Fifo() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& [s, st] : surfaces_)
        if (WpFifoV1* r = st->resource.get())
            r->detach();
}

void Fifo::refreshed(Surface* surface) {
    auto it = surfaces_.find(surface);
    if (it == surfaces_.end() || !it->second->barrier)
        return;
    it->second->barrier = false;
    surface->unlock_first(SurfaceState::LockFifo);
}

// ---- commit timing ---------------------------------------------------------------------

struct CommitTiming::Waiting {
    Surface* surface;
    SurfaceState* state;
    wl_event_source* timer;
    Connection gone;
};

CommitTiming::CommitTiming(wl_display* display) : display_(display) {
    global_ = Global::create<WpCommitTimingManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<WpCommitTimingManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_timer([this](WpCommitTimingManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* t = make<WpCommitTimerV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!t)
                return;
            if (!s) {
                t->detach();
                return;
            }
            if (timers_.contains(s)) {
                self->post_error(uint32_t(WpCommitTimingManagerV1::Error::CommitTimerExists),
                                 "the surface already has one");
                return;
            }
            timers_[s] = t;
            t->on_set_timestamp([s](WpCommitTimerV1* self, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
                if (nsec >= 1000000000) {
                    self->post_error(uint32_t(WpCommitTimerV1::Error::InvalidTimestamp), "nanoseconds out of range");
                    return;
                }
                if (s->pending().target_ns) {
                    self->post_error(uint32_t(WpCommitTimerV1::Error::TimestampExists), "already set for this commit");
                    return;
                }
                const int64_t sec = int64_t((uint64_t(sec_hi) << 32) | sec_lo);
                s->pending_state().target_ns = sec * 1000000000 + nsec;
                s->pending_state().committed |= SurfaceState::Timing;
            });
            auto queued = std::make_shared<Connection>();
            *queued = s->events.queued.connect([this, s](SurfaceState* st) {
                if (!st->target_ns)
                    return;
                const int64_t wait = st->target_ns - now_ns();
                if (wait <= 0)
                    return;
                st->locks |= SurfaceState::LockTimer;
                auto w = std::make_unique<Waiting>(Waiting{s, st, nullptr, {}});
                Waiting* wp = w.get();
                wp->timer = wl_event_loop_add_timer(wl_display_get_event_loop(display_), [](void* data) {
                    auto* wp = static_cast<Waiting*>(data);
                    wl_event_source_remove(wp->timer);
                    wp->timer = nullptr;
                    wp->gone.disconnect();
                    wp->surface->unlock(wp->state, SurfaceState::LockTimer);
                    return 0;
                }, wp);
                wl_event_source_timer_update(wp->timer, int(std::max<int64_t>(1, wait / 1000000)));
                wp->gone = s->events.destroy.connect([wp] {
                    if (wp->timer)
                        wl_event_source_remove(wp->timer);
                    wp->timer = nullptr;
                });
                std::erase_if(waiting_, [](const auto& x) { return !x->timer; });
                waiting_.push_back(std::move(w));
            });
            auto gone = s->events.destroy.connect([this, s, t] {
                timers_.erase(s);
                t->detach();
            });
            t->on_gone([this, s, t, queued, g = std::make_shared<Connection>(std::move(gone))] {
                queued->disconnect();
                g->disconnect();
                if (auto it = timers_.find(s); it != timers_.end() && it->second == t)
                    timers_.erase(it);
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

CommitTiming::~CommitTiming() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& [s, t] : timers_)
        t->detach();
    for (auto& w : waiting_)
        if (w->timer)
            wl_event_source_remove(w->timer);
}

// ---- syncobj -----------------------------------------------------------------------------

namespace {

using TimelinePtr = std::shared_ptr<wlr_drm_syncobj_timeline>;

TimelinePtr adopt(wlr_drm_syncobj_timeline* t) {
    return TimelinePtr(t, [](wlr_drm_syncobj_timeline* x) {
        if (x)
            wlr_drm_syncobj_timeline_unref(x);
    });
}

class TimelineResource : public WpLinuxDrmSyncobjTimelineV1 {
public:
    TimelineResource(wl_client* client, uint32_t version, uint32_t id, TimelinePtr t)
        : WpLinuxDrmSyncobjTimelineV1(client, version, id), timeline(std::move(t)) {}
    TimelinePtr timeline;
};

// Merges `fd` into `*into` (a sync_file), consuming it.
void merge_sync_file(int* into, int fd) {
    if (*into < 0) {
        *into = fd;
        return;
    }
    sync_merge_data data{};
    std::strncpy(data.name, "atrium-release", sizeof data.name - 1);
    data.fd2 = fd;
    if (ioctl(*into, SYNC_IOC_MERGE, &data) == 0) {
        close(*into);
        *into = data.fence;
    }
    close(fd);
}

} // namespace

// A GPU wait turned into an event-loop callback.
struct Syncobj::Waiter {
    wlr_drm_syncobj_timeline_waiter waiter;
    std::function<void()> ready;
    bool fired = false;
};

// The client's release point for one buffer: signalled once the buffer is
// let go and every read of it the compositor queued has finished.
struct Syncobj::Release {
    Syncobj* owner;
    TimelinePtr timeline;
    uint64_t point;
    int merged = -1;  // sync_file of the reads so far
    int pending = 0;  // reads whose fence doesn't exist yet
    bool released = false;
    wlr_buffer* buffer;
    wl_listener release_listener{};

    void finish_if_done() {
        if (!released || pending > 0)
            return;
        bool ok = false;
        if (merged >= 0) {
            ok = wlr_drm_syncobj_timeline_import_sync_file(timeline.get(), point, merged);
            close(merged);
            merged = -1;
        }
        if (!ok)
            wlr_drm_syncobj_timeline_signal(timeline.get(), point);
        std::erase(owner->releases_, this);
        delete this;
    }
};

struct Syncobj::PerSurface {
    Weak<WpLinuxDrmSyncobjSurfaceV1> resource;
    Release* current = nullptr;  // the shown buffer's
    Connection queued, commit, gone;
};

Syncobj::Syncobj(wl_display* display, int drm_fd) : drm_fd_(drm_fd), display_(display) {
    global_ = Global::create<WpLinuxDrmSyncobjManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                            uint32_t id) {
        auto* m = make<WpLinuxDrmSyncobjManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_import_timeline([this](WpLinuxDrmSyncobjManagerV1* self, uint32_t id, int fd) {
            wlr_drm_syncobj_timeline* t = wlr_drm_syncobj_timeline_import(drm_fd_, fd);
            close(fd);
            if (!t) {
                self->post_error(uint32_t(WpLinuxDrmSyncobjManagerV1::Error::InvalidTimeline),
                                 "not a timeline this GPU can use");
                if (auto* r = make<WpLinuxDrmSyncobjTimelineV1>(self->client(), self->version(), id))
                    r->detach();
                return;
            }
            make<TimelineResource>(self->client(), self->version(), id, adopt(t));
        });
        m->on_get_surface([this](WpLinuxDrmSyncobjManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<WpLinuxDrmSyncobjSurfaceV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s) {
                r->detach();
                return;
            }
            if (surfaces_.contains(s)) {
                self->post_error(uint32_t(WpLinuxDrmSyncobjManagerV1::Error::SurfaceExists),
                                 "the surface already has one");
                return;
            }
            auto ps = std::make_unique<PerSurface>();
            PerSurface* p = ps.get();
            p->resource = r;
            surfaces_[s] = std::move(ps);
            auto point = [s, r](WpLinuxDrmSyncobjTimelineV1* tl, uint32_t hi, uint32_t lo, bool acquire) {
                auto* t = dynamic_cast<TimelineResource*>(tl);
                if (!t)
                    return;
                auto& sync = s->pending_state().sync;
                (acquire ? sync.acquire : sync.release) = t->timeline;
                (acquire ? sync.acquire_point : sync.release_point) = (uint64_t(hi) << 32) | lo;
                s->pending_state().committed |= SurfaceState::Sync;
                (void)r;
            };
            r->on_set_acquire_point([point](WpLinuxDrmSyncobjSurfaceV1*, WpLinuxDrmSyncobjTimelineV1* t,
                                            uint32_t hi, uint32_t lo) { point(t, hi, lo, true); });
            r->on_set_release_point([point](WpLinuxDrmSyncobjSurfaceV1*, WpLinuxDrmSyncobjTimelineV1* t,
                                            uint32_t hi, uint32_t lo) { point(t, hi, lo, false); });

            // Checked and held as the commit goes in.
            p->queued = s->events.queued.connect([this, s, r](SurfaceState* st) {
                using E = WpLinuxDrmSyncobjSurfaceV1::Error;
                const bool buffer = (st->committed & SurfaceState::Buffer) && st->buffer;
                const auto& sy = st->sync;
                auto reject = [&](E code, const char* message) {
                    r->post_error(uint32_t(code), message);
                    st->rejected = true;
                };
                if (!buffer) {
                    if (sy.acquire || sy.release)
                        reject(E::NoBuffer, "sync points without a buffer");
                    return;
                }
                if (!sy.acquire)
                    return reject(E::NoAcquirePoint, "a buffer without an acquire point");
                if (!sy.release)
                    return reject(E::NoReleasePoint, "a buffer without a release point");
                if (sy.acquire == sy.release && sy.acquire_point >= sy.release_point)
                    return reject(E::ConflictingPoints, "release point not after the acquire point");
                if (!LinuxDmabuf::is_dmabuf(st->buffer.get()))
                    return reject(E::UnsupportedBuffer, "explicit sync needs a dmabuf");
                // Hold the state until its acquire point exists (the GPU then
                // waits for it to signal).
                bool ready = false;
                if (wlr_drm_syncobj_timeline_check(sy.acquire.get(), sy.acquire_point,
                                                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, &ready) &&
                    ready)
                    return;
                st->locks |= SurfaceState::LockFence;
                auto w = std::make_unique<Waiter>();
                Waiter* wp = w.get();
                wp->ready = [s, st] { s->unlock(st, SurfaceState::LockFence); };
                if (!wlr_drm_syncobj_timeline_waiter_init(&wp->waiter, sy.acquire.get(), sy.acquire_point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                                                          wl_display_get_event_loop(display_),
                                                          [](wlr_drm_syncobj_timeline_waiter* waiter) {
                                                              auto* wp = reinterpret_cast<Waiter*>(waiter);
                                                              wp->fired = true;
                                                              wlr_drm_syncobj_timeline_waiter_finish(waiter);
                                                              wp->ready();
                                                          })) {
                    st->locks &= ~SurfaceState::LockFence;  // can't wait: let the GPU do it
                    return;
                }
                std::erase_if(waiters_, [](const auto& x) { return x->fired; });
                waiters_.push_back(std::move(w));
            });
            // An applied buffer gets its release tracker.
            p->commit = s->events.commit.connect([this, s, p] {
                const auto& sy = s->current().sync;
                if (!(s->current().committed & SurfaceState::Buffer) || !sy.release)
                    return;
                wlr_buffer* b = s->current().buffer.get();
                if (!b)
                    return;
                auto* rel = new Release{this, sy.release, sy.release_point, -1, 0, false, b, {}};
                rel->release_listener.notify = [](wl_listener* l, void*) {
                    Release* rel = wl_container_of(l, rel, release_listener);
                    wl_list_remove(&rel->release_listener.link);
                    wl_list_init(&rel->release_listener.link);
                    rel->released = true;
                    rel->finish_if_done();
                };
                wl_signal_add(&b->events.release, &rel->release_listener);
                releases_.push_back(rel);
                p->current = rel;
            });
            auto drop = [this, s] {
                auto it = surfaces_.find(s);
                if (it == surfaces_.end())
                    return;
                if (auto* r = it->second->resource.get())
                    r->detach();
                surfaces_.erase(it);
            };
            p->gone = s->events.destroy.connect(drop);
            r->on_gone(drop);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Syncobj::~Syncobj() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    for (auto& [s, p] : surfaces_)
        if (auto* r = p->resource.get())
            r->detach();
    for (auto& w : waiters_)
        if (!w->fired)
            wlr_drm_syncobj_timeline_waiter_finish(&w->waiter);
    // Whatever is still held goes back now, so no client waits forever.
    for (Release* r : std::vector(releases_)) {
        wl_list_remove(&r->release_listener.link);
        wl_list_init(&r->release_listener.link);
        r->released = true;
        r->pending = 0;
        r->finish_if_done();
    }
}

void Syncobj::add_release_point(Surface* surface, wlr_drm_syncobj_timeline* timeline, uint64_t point) {
    auto it = surfaces_.find(surface);
    if (it == surfaces_.end() || !it->second->current || !timeline)
        return;
    Release* rel = it->second->current;
    if (std::ranges::find(releases_, rel) == releases_.end())
        return;
    ++rel->pending;
    auto w = std::make_unique<Waiter>();
    Waiter* wp = w.get();
    TimelinePtr keep(wlr_drm_syncobj_timeline_ref(timeline), [](wlr_drm_syncobj_timeline* x) {
        wlr_drm_syncobj_timeline_unref(x);
    });
    wp->ready = [this, rel, keep, point] {
        if (std::ranges::find(releases_, rel) == releases_.end())
            return;
        const int fd = wlr_drm_syncobj_timeline_export_sync_file(keep.get(), point);
        if (fd >= 0)
            merge_sync_file(&rel->merged, fd);
        --rel->pending;
        rel->finish_if_done();
    };
    if (!wlr_drm_syncobj_timeline_waiter_init(&wp->waiter, timeline, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, wl_display_get_event_loop(display_),
                                              [](wlr_drm_syncobj_timeline_waiter* waiter) {
                                                  auto* wp = reinterpret_cast<Waiter*>(waiter);
                                                  wp->fired = true;
                                                  wlr_drm_syncobj_timeline_waiter_finish(waiter);
                                                  wp->ready();
                                              })) {
        --rel->pending;
        return;
    }
    std::erase_if(waiters_, [](const auto& x) { return x->fired; });
    waiters_.push_back(std::move(w));
}

} // namespace atrium::wl
