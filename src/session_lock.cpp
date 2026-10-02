#include "session_lock.hpp"

#include "output.hpp"
#include "overview.hpp"
#include "seat.hpp"
#include "server.hpp"

namespace atrium {

SessionLock::SessionLock(Server& srv, wl::Lock* l) : server(srv), lock(l) {
    server.focus_view(nullptr);
    tree = scene::Tree::create(server.layer(Layer::Lock));
    server.lock = this;
    server.overview->close_now();
    server.locked = true;

    // Nothing below the lock keeps a grab or pointer focus.
    server.seat->cancel_grab();
    server.seat->refresh_pointer();

    new_surface_ = lock->events.new_surface.connect([this](wl::LockSurface* s) { new_surface(s); });
    unlock_ = lock->events.unlock.connect([this] { finish(true); });
    destroy_ = lock->events.destroy.connect([this] { finish(false); });

    lock->locked();
}

SessionLock::~SessionLock() {
    tree->destroy();
}

void SessionLock::new_surface(wl::LockSurface* ls) {
    auto* o = ls->output() ? static_cast<Output*>(ls->output()->data) : nullptr;
    if (!o)
        return;
    auto* st = scene::subsurface_tree_create(tree, ls->surface());
    ls->surface()->data = st;
    o->lock_surface = ls;

    st->set_position(o->box.x, o->box.y);
    ls->configure(uint32_t(o->box.width), uint32_t(o->box.height));

    // These connections live on the Output and can fire after this lock is
    // gone (unlock deletes the lock before its surfaces are destroyed), so
    // they hold the Server, never `this`.
    Server& srv = server;
    o->lock_surface_commit = ls->surface()->events.commit.connect([&srv, o] {
        if (!o->lock_surface || !o->lock_surface->surface() || !o->lock_surface->surface()->mapped())
            return;
        o->lock_surface_commit.disconnect();
        srv.seat->refresh_pointer();
    });
    o->lock_surface_destroy = ls->destroy_signal.connect([&srv, o] {
        wl::LockSurface* gone = o->lock_surface;
        o->lock_surface = nullptr;
        o->lock_surface_commit.disconnect();
        o->lock_surface_destroy.disconnect();

        if (!gone || gone->surface() != srv.wl->seat->keyboard_focus())
            return;
        // Hand the keyboard to another lock surface, or back to the desktop.
        wl::LockSurface* next = nullptr;
        if (srv.locked && srv.lock)
            for (wl::LockSurface* s : srv.lock->lock->surfaces())
                if (s != gone && s->surface())
                    next = s;
        if (next)
            srv.seat->keyboard_enter(next->surface());
        else if (!srv.locked)
            srv.focus_top();
        else
            srv.seat->clear_keyboard_focus();
    });

    if (o == server.focused_output)
        server.seat->keyboard_enter(ls->surface());
}

// A lock destroyed without unlocking (the locker crashed) leaves the session
// locked: the lock background stays up and a new locker can take over.
void SessionLock::finish(bool unlocked) {
    server.seat->clear_keyboard_focus();
    server.locked = !unlocked;
    server.lock = nullptr;
    if (unlocked) {
        server.locked_bg->set_enabled(false);
        server.focus_top();
        server.seat->refresh_pointer();
    }
    delete this;
}

} // namespace atrium
