// GBM buffers and swapchains, after wlroots' render/allocator/gbm.c and
// render/swapchain.c (MIT).
#include "backend/allocator.hpp"

#include "wlr.hpp"

extern "C" {
#include <gbm.h>
#include <wlr/render/dmabuf.h>
}

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

namespace atrium::backend {

namespace {

struct GbmBuffer {
    wlr_buffer base;
    gbm_bo* bo;
    wlr_dmabuf_attributes dmabuf;
};

GbmBuffer* gbm_of(wlr_buffer* b) {
    return reinterpret_cast<GbmBuffer*>(b);
}

const wlr_buffer_impl kGbmBufferImpl = {
    .destroy =
        [](wlr_buffer* b) {
            GbmBuffer* g = gbm_of(b);
            wlr_buffer_finish(b);
            wlr_dmabuf_attributes_finish(&g->dmabuf);
            gbm_bo_destroy(g->bo);
            delete g;
        },
    .get_dmabuf =
        [](wlr_buffer* b, wlr_dmabuf_attributes* out) {
            *out = gbm_of(b)->dmabuf;
            return true;
        },
};

// The buffer object's planes as a dmabuf.
bool export_dmabuf(gbm_bo* bo, wlr_dmabuf_attributes* out) {
    wlr_dmabuf_attributes a{};
    a.n_planes = gbm_bo_get_plane_count(bo);
    if (a.n_planes <= 0 || a.n_planes > WLR_DMABUF_MAX_PLANES)
        return false;
    a.width = int32_t(gbm_bo_get_width(bo));
    a.height = int32_t(gbm_bo_get_height(bo));
    a.format = gbm_bo_get_format(bo);
    a.modifier = gbm_bo_get_modifier(bo);
    for (int i = 0; i < a.n_planes; ++i) {
        a.fd[i] = gbm_bo_get_fd_for_plane(bo, i);
        if (a.fd[i] < 0) {
            for (int j = 0; j < i; ++j)
                close(a.fd[j]);
            return false;
        }
        a.offset[i] = gbm_bo_get_offset(bo, i);
        a.stride[i] = gbm_bo_get_stride_for_plane(bo, i);
    }
    *out = a;
    return true;
}

} // namespace

std::unique_ptr<Allocator> Allocator::create(int drm_fd) {
    const int fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return nullptr;
    gbm_device* gbm = gbm_create_device(fd);
    if (!gbm) {
        close(fd);
        wlr_log(WLR_ERROR, "allocator: no GBM device");
        return nullptr;
    }
    std::unique_ptr<Allocator> a(new Allocator());
    a->fd_ = fd;
    a->gbm_ = gbm;
    return a;
}

Allocator::~Allocator() {
    if (gbm_)
        gbm_device_destroy(gbm_);
    if (fd_ >= 0)
        close(fd_);
}

wlr_buffer* Allocator::allocate(int width, int height, uint32_t format, const std::vector<uint64_t>& modifiers) {
    const bool implicit = modifiers.empty() ||
                          (modifiers.size() == 1 && modifiers[0] == DRM_FORMAT_MOD_INVALID);
    gbm_bo* bo = nullptr;
    for (uint32_t flags : {uint32_t(GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT), uint32_t(GBM_BO_USE_RENDERING)}) {
        if (implicit) {
            bo = gbm_bo_create(gbm_, uint32_t(width), uint32_t(height), format, flags);
        } else {
            std::vector<uint64_t> mods;
            std::ranges::copy_if(modifiers, std::back_inserter(mods), [](uint64_t m) { return m != DRM_FORMAT_MOD_INVALID; });
            bo = gbm_bo_create_with_modifiers2(gbm_, uint32_t(width), uint32_t(height), format, mods.data(),
                                               unsigned(mods.size()), flags);
        }
        if (bo)
            break;
    }
    if (!bo) {
        wlr_log(WLR_ERROR, "allocator: no %dx%d buffer of format 0x%x", width, height, format);
        return nullptr;
    }
    auto* g = new GbmBuffer{};
    g->bo = bo;
    if (!export_dmabuf(bo, &g->dmabuf)) {
        gbm_bo_destroy(bo);
        delete g;
        return nullptr;
    }
    // Made without modifiers, its layout is the driver's implicit one, whatever
    // GBM reports (wlroots does the same).
    if (implicit)
        g->dmabuf.modifier = DRM_FORMAT_MOD_INVALID;
    wlr_buffer_init(&g->base, &kGbmBufferImpl, width, height);
    return &g->base;
}

// ---- Swapchain ------------------------------------------------------------------------

Swapchain::Swapchain(Allocator& allocator, int w, int h, uint32_t f, std::vector<uint64_t> m)
    : width(w), height(h), format(f), modifiers(std::move(m)), allocator_(allocator) {
    for (Slot& s : slots_) {
        s.owner = this;
        wl_list_init(&s.release.link);
    }
}

Swapchain::~Swapchain() {
    for (Slot& s : slots_) {
        wl_list_remove(&s.release.link);
        if (s.buffer)
            wlr_buffer_drop(s.buffer);
    }
}

bool Swapchain::has(const wlr_buffer* b) const {
    return std::ranges::any_of(slots_, [b](const Slot& s) { return s.buffer == b; });
}

wlr_buffer* Swapchain::acquire() {
    Slot* free = nullptr;
    for (Slot& s : slots_)
        if (s.buffer && !s.acquired) {
            free = &s;
            break;
        }
    if (!free)
        for (Slot& s : slots_)
            if (!s.buffer) {
                s.buffer = allocator_.allocate(width, height, format, modifiers);
                if (!s.buffer)
                    return nullptr;
                free = &s;
                break;
            }
    if (!free)
        return nullptr;  // all held
    free->acquired = true;
    // Free again when the last lock goes.
    wl_list_remove(&free->release.link);
    free->release.notify = [](wl_listener* l, void*) {
        Slot* s = wl_container_of(l, s, release);
        s->acquired = false;
        wl_list_remove(&s->release.link);
        wl_list_init(&s->release.link);
    };
    wl_signal_add(&free->buffer->events.release, &free->release);
    return wlr_buffer_lock(free->buffer);
}

} // namespace atrium::backend
