#include "wl/session_lock.hpp"

#include "wl/output.hpp"

#include <algorithm>

namespace atrium::wl {

LockSurface::LockSurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, Output* output)
    : ExtSessionLockSurfaceV1(client, version, id), surface_(surface), output_(output) {
    surface_gone_ = surface->events.destroy.connect([this] { surface_ = nullptr; });
    on_ack_configure([this](ExtSessionLockSurfaceV1*, uint32_t serial) {
        auto it = std::ranges::find_if(sent_, [serial](const Sent& s) { return s.serial == serial; });
        if (it == sent_.end()) {
            post_error(uint32_t(Error::InvalidSerial), "no configure with that serial");
            return;
        }
        width_ = it->width;
        height_ = it->height;
        acked_ = true;
        sent_.erase(sent_.begin(), it + 1);
    });
}

LockSurface::~LockSurface() {
    if (surface_) {
        surface_->unmap();
        surface_->clear_role(this);
    }
    destroy_signal.emit();
}

uint32_t LockSurface::configure(uint32_t width, uint32_t height) {
    if (!client())
        return 0;
    const uint32_t serial = wl_display_next_serial(wl_client_get_display(client()));
    sent_.push_back({serial, width, height});
    send_configure(serial, width, height);
    return serial;
}

bool LockSurface::precommit(Surface& s) {
    const SurfaceState& p = s.pending();
    if (!acked_) {
        post_error(uint32_t(Error::CommitBeforeFirstAck), "a commit before the first configure was acked");
        return false;
    }
    if ((p.committed & SurfaceState::Buffer) && !p.buffer) {
        post_error(uint32_t(Error::NullBuffer), "a lock surface can't be emptied");
        return false;
    }
    return true;
}

void LockSurface::commit(Surface& s) {
    const SurfaceState& c = s.current();
    // The size it was told, exactly: a lock screen can't leave a gap.
    if (c.buffer_width && (uint32_t(c.width) != width_ || uint32_t(c.height) != height_)) {
        post_error(uint32_t(Error::DimensionsMismatch), "the buffer isn't the configured size");
        return;
    }
    if (!s.mapped() && s.buffer())
        s.map();
}

Lock::Lock(wl_client* client, uint32_t version, uint32_t id) : ExtSessionLockV1(client, version, id) {
    on_get_lock_surface([this](ExtSessionLockV1*, uint32_t id, wl_resource* surface_res, wl_resource* output_res) {
        Surface* surface = Surface::from(surface_res);
        Output* output = Output::from(output_res);
        if (!surface)
            return;
        auto* ls = make<LockSurface>(this->client(), this->version(), id, surface, output);
        if (!ls)
            return;
        if ((surface->role_name() && surface->role_name() != LockSurface::kRole) || surface->role()) {
            post_error(uint32_t(Error::Role), "the surface has another role");
            return;
        }
        if (surface->buffer() || surface->pending().buffer) {
            post_error(uint32_t(Error::AlreadyConstructed), "the surface already has a buffer");
            return;
        }
        if (output && std::ranges::any_of(surfaces_, [output](LockSurface* s) { return s->output() == output; })) {
            post_error(uint32_t(Error::DuplicateOutput), "that output already has a lock surface");
            return;
        }
        if (finished_ || !output) {
            ls->detach();  // a lock that was refused, or a screen that went
            return;
        }
        surface->set_role(ls, nullptr, 0);
        surfaces_.push_back(ls);
        ls->on_gone([this, ls] { std::erase(surfaces_, ls); });
        events.new_surface.emit(ls);
    });
    on_unlock_and_destroy([this](ExtSessionLockV1*) {
        if (!locked_) {
            post_error(uint32_t(Error::InvalidUnlock), "unlock before the session was locked");
            return;
        }
        unlocked_ = true;
        events.unlock.emit();
    });
    on_destroy([this](ExtSessionLockV1*) {
        if (locked_ && !finished_)
            post_error(uint32_t(Error::InvalidDestroy), "a lock is undone with unlock_and_destroy");
    });
}

Lock::~Lock() {
    // Its surfaces may outlive it (the client destroys them after): they
    // mustn't reach back.
    for (LockSurface* ls : surfaces_)
        ls->on_gone(nullptr);
    events.destroy.emit();
}

void Lock::locked() {
    if (locked_ || finished_)
        return;
    locked_ = true;
    send_locked();
}

void Lock::finish() {
    if (finished_)
        return;
    finished_ = true;
    send_finished();
}

SessionLockManager::SessionLockManager(wl_display* display) {
    global_ = Global::create<ExtSessionLockManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<ExtSessionLockManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_lock([this](ExtSessionLockManagerV1* self, uint32_t id) {
            if (auto* lock = make<Lock>(self->client(), self->version(), id))
                new_lock.emit(lock);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

SessionLockManager::~SessionLockManager() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
}

} // namespace atrium::wl
