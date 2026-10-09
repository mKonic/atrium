#include "system_bell.hpp"

#include "server.hpp"
#include "view.hpp"

#include <canberra.h>

#include <ctime>

namespace atrium {

namespace {

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

SystemBell::SystemBell(Server& server) : server_(server), bell_(wlr_xdg_system_bell_v1_create(server.display, 1)) {
    if (bell_)
        ring_.connect(&bell_->events.ring, [this](wlr_xdg_system_bell_v1_ring_event* e) { ring(e); });
}

SystemBell::~SystemBell() {
    if (!sound_)
        return;
    {
        std::lock_guard lock(sound_->mutex);
        sound_->quit = true;
    }
    sound_->wake.notify_one();
}

void SystemBell::ring(wlr_xdg_system_bell_v1_ring_event* e) {
    // The window that rang; with no surface, the focused one if it's the
    // ringing app's.
    View* view = nullptr;
    if (e->surface) {
        view = Server::owner_of(e->surface).view;
    } else if (View* f = server_.focused_view; f && f->surface() &&
                                                wl_resource_get_client(f->surface()->resource) == e->client) {
        view = f;
    }
    if (!view && !e->surface)
        return;  // another app's ring, with nothing of its own (KWin rings nothing)
    if (server_.config.system_bell)
        play();
    if (view && view != server_.focused_view && !view->urgent) {
        view->urgent = true;
        server_.notify_window(*view, "changed");
    }
}

void SystemBell::play() {
    const int64_t now = now_ms();
    if (now - last_sound_ms_ < 100)
        return;
    last_sound_ms_ = now;
    if (!sound_) {
        sound_ = std::make_shared<Sound>();
        std::thread([sound = sound_] {
            ca_context* ctx = nullptr;
            for (;;) {
                {
                    std::unique_lock lock(sound->mutex);
                    sound->wake.wait(lock, [&] { return sound->pending || sound->quit; });
                    if (sound->quit)
                        break;
                    sound->pending = false;  // rings while it's busy don't pile up
                }
                if (!ctx && ca_context_create(&ctx) == CA_SUCCESS)
                    ca_context_change_props(ctx, CA_PROP_APPLICATION_NAME, "atrium", CA_PROP_APPLICATION_ID,
                                            "atrium", nullptr);
                if (ctx)
                    ca_context_play(ctx, 0, CA_PROP_EVENT_ID, "bell", CA_PROP_MEDIA_ROLE, "event", nullptr);
            }
            if (ctx)
                ca_context_destroy(ctx);
        }).detach();
    }
    {
        std::lock_guard lock(sound_->mutex);
        sound_->pending = true;
    }
    sound_->wake.notify_one();
}

} // namespace atrium
