#include "session_lock.hpp"

#include "output.hpp"
#include "overview.hpp"
#include "seat.hpp"
#include "server.hpp"

namespace atrium {

SessionLock::SessionLock(Server& srv, wlr_session_lock_v1* lock) : server(srv), wlr(lock) {
    server.focus_view(nullptr);
    tree = wlr_scene_tree_create(server.layer(Layer::Lock));
    server.lock = this;
    server.overview->close_now();
    server.locked = true;

    // Nothing below the lock keeps a grab or pointer focus.
    server.seat->cancel_grab();
    server.seat->refresh_pointer();

    new_surface_.connect(&wlr->events.new_surface,
        [this](wlr_session_lock_surface_v1* s) { new_surface(s); });
    unlock_.connect(&wlr->events.unlock, [this](void*) { finish(true); });
    destroy_.connect(&wlr->events.destroy, [this](void*) { finish(false); });

    wlr_session_lock_v1_send_locked(wlr);
}

SessionLock::~SessionLock() {
    wlr_scene_node_destroy(&tree->node);
}

void SessionLock::new_surface(wlr_session_lock_surface_v1* ls) {
    auto* o = static_cast<Output*>(ls->output->data);
    auto* st = wlr_scene_subsurface_tree_create(tree, ls->surface);
    ls->surface->data = st;
    o->lock_surface = ls;

    wlr_scene_node_set_position(&st->node, o->box.x, o->box.y);
    wlr_session_lock_surface_v1_configure(ls, o->box.width, o->box.height);

    // These listeners live on the Output and can fire after this lock is gone
    // (unlock deletes the lock before its surfaces are destroyed), so they
    // hold the Server, never `this`.
    Server& srv = server;
    o->lock_surface_commit.connect(&ls->surface->events.commit, [&srv, o](void*) {
        if (!o->lock_surface || !o->lock_surface->surface->mapped)
            return;
        o->lock_surface_commit.disconnect();
        srv.seat->refresh_pointer();
    });
    o->lock_surface_destroy.connect(&ls->events.destroy, [&srv, o](void*) {
        wlr_session_lock_surface_v1* gone = o->lock_surface;
        o->lock_surface = nullptr;
        o->lock_surface_commit.disconnect();
        o->lock_surface_destroy.disconnect();

        if (gone->surface != srv.seat->wlr->keyboard_state.focused_surface)
            return;
        // Hand the keyboard to another lock surface, or back to the desktop.
        if (srv.locked && srv.lock && !wl_list_empty(&srv.lock->wlr->surfaces)) {
            wlr_session_lock_surface_v1* next =
                wl_container_of(srv.lock->wlr->surfaces.next, next, link);
            srv.seat->keyboard_enter(next->surface);
        } else if (!srv.locked) {
            srv.focus_top();
        } else {
            srv.seat->clear_keyboard_focus();
        }
    });

    if (o == server.focused_output)
        server.seat->keyboard_enter(ls->surface);
}

// A lock destroyed without unlocking (the locker crashed) leaves the session
// locked: the lock background stays up and a new locker can take over.
void SessionLock::finish(bool unlocked) {
    server.seat->clear_keyboard_focus();
    server.locked = !unlocked;
    server.lock = nullptr;
    if (unlocked) {
        wlr_scene_node_set_enabled(&server.locked_bg->node, false);
        server.focus_top();
        server.seat->refresh_pointer();
    }
    delete this;
}

} // namespace atrium
