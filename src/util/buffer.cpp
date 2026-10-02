#include "util/buffer.hpp"

#include "render/formats.hpp"
#include "util/log.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>

namespace atrium {

void dmabuf_attributes_finish(DmabufAttributes* a) {
    for (int i = 0; i < a->n_planes; ++i) {
        if (a->fd[i] >= 0)
            close(a->fd[i]);
        a->fd[i] = -1;
    }
    a->n_planes = 0;
}

bool dmabuf_attributes_copy(DmabufAttributes* dst, const DmabufAttributes* src) {
    *dst = *src;
    for (int i = 0; i < src->n_planes; ++i) {
        dst->fd[i] = fcntl(src->fd[i], F_DUPFD_CLOEXEC, 0);
        if (dst->fd[i] < 0) {
            for (int j = 0; j < i; ++j)
                close(dst->fd[j]);
            *dst = {};
            return false;
        }
    }
    return true;
}

// ---- addons ----------------------------------------------------------------

void addon_set_init(AddonSet* set) {
    wl_list_init(&set->addons);
}

void addon_set_finish(AddonSet* set) {
    while (!wl_list_empty(&set->addons)) {
        wl_list* link = set->addons.next;
        Addon* addon = wl_container_of(link, addon, link);
        const AddonInterface* impl = addon->impl;
        impl->destroy(addon);
        if (set->addons.next == link) {
            alog(Log::Error, "an addon (%s) stayed on after its destroy", impl->name);
            std::abort();
        }
    }
}

void addon_init(Addon* addon, AddonSet* set, const void* owner, const AddonInterface* impl) {
    assert(impl);
    assert(!addon_find(set, owner, impl) && "two addons of one kind for one owner");
    addon->impl = impl;
    addon->owner = owner;
    wl_list_insert(&set->addons, &addon->link);
}

void addon_finish(Addon* addon) {
    wl_list_remove(&addon->link);
}

Addon* addon_find(AddonSet* set, const void* owner, const AddonInterface* impl) {
    Addon* addon;
    wl_list_for_each(addon, &set->addons, link) {
        if (addon->owner == owner && addon->impl == impl)
            return addon;
    }
    return nullptr;
}

// ---- buffers ---------------------------------------------------------------

void buffer_init(Buffer* buffer, const BufferImpl* impl, int width, int height) {
    assert(impl->destroy);
    assert(!impl->begin_data_ptr_access == !impl->end_data_ptr_access);
    buffer->impl = impl;
    buffer->width = width;
    buffer->height = height;
    buffer->dropped = false;
    buffer->n_locks = 0;
    buffer->accessing_data_ptr = false;
    wl_signal_init(&buffer->events.destroy);
    wl_signal_init(&buffer->events.release);
    addon_set_init(&buffer->addons);
}

void buffer_finish(Buffer* buffer) {
    wl_signal_emit_mutable(&buffer->events.destroy, nullptr);
    addon_set_finish(&buffer->addons);
    assert(wl_list_empty(&buffer->events.destroy.listener_list));
    assert(wl_list_empty(&buffer->events.release.listener_list));
}

namespace {

void consider_destroy(Buffer* buffer) {
    if (!buffer->dropped || buffer->n_locks > 0)
        return;
    assert(!buffer->accessing_data_ptr);
    buffer->impl->destroy(buffer);
}

} // namespace

void buffer_drop(Buffer* buffer) {
    if (!buffer)
        return;
    assert(!buffer->dropped);
    buffer->dropped = true;
    consider_destroy(buffer);
}

Buffer* buffer_lock(Buffer* buffer) {
    ++buffer->n_locks;
    return buffer;
}

void buffer_unlock(Buffer* buffer) {
    if (!buffer)
        return;
    assert(buffer->n_locks > 0);
    if (--buffer->n_locks == 0)
        wl_signal_emit_mutable(&buffer->events.release, nullptr);
    consider_destroy(buffer);
}

bool buffer_get_dmabuf(Buffer* buffer, DmabufAttributes* attribs) {
    return buffer->impl->get_dmabuf && buffer->impl->get_dmabuf(buffer, attribs);
}

bool buffer_get_shm(Buffer* buffer, ShmAttributes* attribs) {
    return buffer->impl->get_shm && buffer->impl->get_shm(buffer, attribs);
}

bool buffer_begin_data_ptr_access(Buffer* buffer, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
    assert(!buffer->accessing_data_ptr);
    if (!buffer->impl->begin_data_ptr_access || !buffer->impl->begin_data_ptr_access(buffer, flags, data, format, stride))
        return false;
    buffer->accessing_data_ptr = true;
    return true;
}

void buffer_end_data_ptr_access(Buffer* buffer) {
    assert(buffer->accessing_data_ptr);
    buffer->impl->end_data_ptr_access(buffer);
    buffer->accessing_data_ptr = false;
}

uint32_t buffer_drm_format(Buffer* buffer) {
    DmabufAttributes dmabuf;
    ShmAttributes shm;
    if (buffer_get_dmabuf(buffer, &dmabuf))
        return dmabuf.format;
    if (buffer_get_shm(buffer, &shm))
        return shm.format;
    return DRM_FORMAT_INVALID;
}

bool buffer_is_opaque(Buffer* buffer) {
    uint32_t format = buffer_drm_format(buffer);
    if (format == DRM_FORMAT_INVALID) {
        void* data;
        size_t stride;
        if (!buffer_begin_data_ptr_access(buffer, BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride))
            return false;
        bool opaque = false;
        // single-pixel-buffer-v1: opaque by its alpha byte.
        if (buffer->width == 1 && buffer->height == 1 && format == DRM_FORMAT_ARGB8888)
            opaque = static_cast<const uint8_t*>(data)[3] == 0xFF;
        buffer_end_data_ptr_access(buffer);
        if (opaque)
            return true;
    }
    return !render::drm_format_has_alpha(format);
}

} // namespace atrium
