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

#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <algorithm>

namespace atrium::backend {

namespace {

struct GbmBuffer {
    wlr_buffer base;
    gbm_bo* bo;
    wlr_dmabuf_attributes dmabuf;
};

// A dumb buffer: CPU-written, scanned out, nothing renders into it.
struct DumbBuffer {
    wlr_buffer base;
    int drm_fd;
    uint32_t handle;
    void* map;
    size_t size;
    wlr_dmabuf_attributes dmabuf;
};

DumbBuffer* dumb_of(wlr_buffer* b) {
    return reinterpret_cast<DumbBuffer*>(b);
}

const wlr_buffer_impl kDumbBufferImpl = {
    .destroy =
        [](wlr_buffer* b) {
            DumbBuffer* d = dumb_of(b);
            wlr_buffer_finish(b);
            wlr_dmabuf_attributes_finish(&d->dmabuf);
            munmap(d->map, d->size);
            drmModeDestroyDumbBuffer(d->drm_fd, d->handle);
            delete d;
        },
    .get_dmabuf =
        [](wlr_buffer* b, wlr_dmabuf_attributes* out) {
            *out = dumb_of(b)->dmabuf;
            return true;
        },
    .begin_data_ptr_access =
        [](wlr_buffer* b, uint32_t, void** data, uint32_t* format, size_t* stride) {
            DumbBuffer* d = dumb_of(b);
            *data = d->map;
            *format = d->dmabuf.format;
            *stride = d->dmabuf.stride[0];
            return true;
        },
    .end_data_ptr_access = [](wlr_buffer*) {},
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

std::unique_ptr<Allocator> Allocator::create_dumb(int drm_fd) {
    const int fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return nullptr;
    uint64_t cap = 0;
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) || !cap) {
        close(fd);
        return nullptr;
    }
    std::unique_ptr<Allocator> a(new Allocator());
    a->fd_ = fd;
    return a;
}

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
    if (!gbm_)
        return allocate_dumb(width, height, format);
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

namespace atrium::backend {

wlr_buffer* Allocator::allocate_dumb(int width, int height, uint32_t format) {
    // 32 bits a pixel: what the screens take from a CPU.
    if (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XBGR8888 &&
        format != DRM_FORMAT_ABGR8888)
        return nullptr;
    uint32_t handle = 0, stride = 0;
    uint64_t size = 0;
    if (drmModeCreateDumbBuffer(fd_, uint32_t(width), uint32_t(height), 32, 0, &handle, &stride, &size) != 0) {
        wlr_log(WLR_ERROR, "allocator: no %dx%d dumb buffer", width, height);
        return nullptr;
    }
    uint64_t offset = 0;
    void* map = MAP_FAILED;
    if (drmModeMapDumbBuffer(fd_, handle, &offset) == 0)
        map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, off_t(offset));
    int prime = -1;
    if (map == MAP_FAILED || drmPrimeHandleToFD(fd_, handle, DRM_CLOEXEC | DRM_RDWR, &prime) != 0) {
        if (map != MAP_FAILED)
            munmap(map, size);
        drmModeDestroyDumbBuffer(fd_, handle);
        return nullptr;
    }
    auto* d = new DumbBuffer{};
    wlr_buffer_init(&d->base, &kDumbBufferImpl, width, height);
    d->drm_fd = fd_;
    d->handle = handle;
    d->map = map;
    d->size = size_t(size);
    d->dmabuf.width = width;
    d->dmabuf.height = height;
    d->dmabuf.format = format;
    d->dmabuf.modifier = DRM_FORMAT_MOD_LINEAR;
    d->dmabuf.n_planes = 1;
    d->dmabuf.fd[0] = prime;
    d->dmabuf.stride[0] = stride;
    return &d->base;
}

} // namespace atrium::backend
