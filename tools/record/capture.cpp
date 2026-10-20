#include "capture.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <span>

namespace atrium::record {

namespace {

const wl_output_listener kOutput = {
    .geometry = [](void* data, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*,
                   int32_t transform) { static_cast<Screen*>(data)->transform = transform; },
    .mode = [](void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
        if (!(flags & WL_OUTPUT_MODE_CURRENT))
            return;
        auto* s = static_cast<Screen*>(data);
        s->width = w;
        s->height = h;
        s->refresh_mhz = refresh;
    },
    .done = [](void*, wl_output*) {},
    .scale = [](void* data, wl_output*, int32_t scale) { static_cast<Screen*>(data)->scale = scale; },
    .name = [](void* data, wl_output*, const char* n) { static_cast<Screen*>(data)->name = n; },
    .description = [](void*, wl_output*, const char*) {},
};

const zxdg_output_v1_listener kXdgOutput = {
    .logical_position = [](void* data, zxdg_output_v1*, int32_t x, int32_t y) {
        auto* s = static_cast<Screen*>(data);
        s->x = x;
        s->y = y;
    },
    .logical_size = [](void* data, zxdg_output_v1*, int32_t w, int32_t h) {
        auto* s = static_cast<Screen*>(data);
        s->logical_width = w;
        s->logical_height = h;
    },
    .done = [](void*, zxdg_output_v1*) {},
    .name = [](void*, zxdg_output_v1*, const char*) {},
    .description = [](void*, zxdg_output_v1*, const char*) {},
};

int64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Best first: opaque before alpha (nothing to blend), then the layouts GPUs
// scan out most.
int rank(uint32_t f, bool ten_bit) {
    static constexpr uint32_t eight[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888};
    static constexpr uint32_t ten[] = {DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ARGB2101010,
                                       DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR16161616F, DRM_FORMAT_ABGR16161616F};
    int r = 0;
    for (uint32_t e : ten_bit ? std::span<const uint32_t>(ten) : std::span<const uint32_t>(eight)) {
        if (e == f)
            return r;
        r++;
    }
    for (uint32_t e : ten_bit ? std::span<const uint32_t>(eight) : std::span<const uint32_t>(ten)) {
        if (e == f)
            return 100 + r;
        r++;
    }
    return INT_MAX;
}

} // namespace

const wl_registry_listener Capture::kRegistry = {
    .global = [](void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
        auto* c = static_cast<Capture*>(data);
        auto is = [iface](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
        if (is(wl_output_interface) && version >= 4) {
            auto s = std::make_unique<Screen>();
            s->wl = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 4));
            wl_output_add_listener(s->wl, &kOutput, s.get());
            c->screens_.push_back(std::move(s));
        } else if (is(ext_image_copy_capture_manager_v1_interface)) {
            c->copy_ = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1));
        } else if (is(ext_output_image_capture_source_manager_v1_interface)) {
            c->sources_ = static_cast<ext_output_image_capture_source_manager_v1*>(
                wl_registry_bind(reg, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        } else if (is(zwp_linux_dmabuf_v1_interface) && version >= 3) {
            c->dmabuf_ = static_cast<zwp_linux_dmabuf_v1*>(
                wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, std::min<uint32_t>(version, 4)));
        } else if (is(zxdg_output_manager_v1_interface) && version >= 2) {
            c->xdg_ = static_cast<zxdg_output_manager_v1*>(
                wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, std::min<uint32_t>(version, 3)));
        }
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

const ext_image_copy_capture_session_v1_listener Capture::kSession = {
    .buffer_size = [](void* data, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto* c = static_cast<Capture*>(data);
        if (c->constraints_done_ && (int(w) != c->width_ || int(h) != c->height_))
            c->reallocate_ = true;
        c->width_ = int(w);
        c->height_ = int(h);
    },
    .shm_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t) {},
    .dmabuf_device = [](void* data, ext_image_copy_capture_session_v1*, wl_array* dev) {
        auto* c = static_cast<Capture*>(data);
        if (dev->size == sizeof(dev_t))
            std::memcpy(&c->device_, dev->data, sizeof(dev_t));
    },
    .dmabuf_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t format, wl_array* mods) {
        auto* c = static_cast<Capture*>(data);
        if (c->constraints_done_) {
            // A new set of constraints: start over.
            c->formats_.clear();
            c->constraints_done_ = false;
            c->reallocate_ = true;
        }
        Format f{format, {}};
        const auto* m = static_cast<const uint64_t*>(mods->data);
        f.modifiers.assign(m, m + mods->size / sizeof(uint64_t));
        c->formats_.push_back(std::move(f));
    },
    .done = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<Capture*>(data)->constraints_done_ = true; },
    .stopped = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<Capture*>(data)->stopped_ = true; },
};

const ext_image_copy_capture_frame_v1_listener Capture::kFrame = {
    .transform = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t t) {
        static_cast<Capture*>(data)->pending_transform_ = int32_t(t);
    },
    .damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .ready = [](void* data, ext_image_copy_capture_frame_v1* f) {
        auto* c = static_cast<Capture*>(data);
        ext_image_copy_capture_frame_v1_destroy(f);
        c->frame_ = nullptr;
        c->latest_ = c->pending_;
        c->pending_ = -1;
        c->frame_transform_ = c->pending_transform_;
        c->frames_++;
        // At the screen's rate (or above) the next one right away: one copy
        // per refresh. Below it, asked for so it lands about a refresh before
        // the clock samples it, not copying frames that are never recorded.
        c->next_request_ns_ = monotonic_ns() + std::max<int64_t>(0, c->interval_ns_ - c->refresh_ns_ * 3 / 2);
    },
    .failed = [](void* data, ext_image_copy_capture_frame_v1* f, uint32_t reason) {
        auto* c = static_cast<Capture*>(data);
        ext_image_copy_capture_frame_v1_destroy(f);
        c->frame_ = nullptr;
        c->pending_ = -1;
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)
            c->stopped_ = true;
        else if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS)
            c->reallocate_ = true;
        c->next_request_ns_ = monotonic_ns();
    },
};

Capture::~Capture() {
    if (frame_)
        ext_image_copy_capture_frame_v1_destroy(frame_);
    free_pool();
    if (session_)
        ext_image_copy_capture_session_v1_destroy(session_);
    if (gbm_)
        gbm_device_destroy(gbm_);
    if (gbm_fd_ >= 0)
        close(gbm_fd_);
    for (auto& s : screens_) {
        if (s->xdg)
            zxdg_output_v1_destroy(s->xdg);
        wl_output_release(s->wl);
    }
    if (xdg_)
        zxdg_output_manager_v1_destroy(xdg_);
    if (dmabuf_)
        zwp_linux_dmabuf_v1_destroy(dmabuf_);
    if (copy_)
        ext_image_copy_capture_manager_v1_destroy(copy_);
    if (sources_)
        ext_output_image_capture_source_manager_v1_destroy(sources_);
    if (registry_)
        wl_registry_destroy(registry_);
    if (display_)
        wl_display_disconnect(display_);
}

bool Capture::connect(std::string& error) {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        error = "no Wayland display";
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistry, this);
    wl_display_roundtrip(display_);
    if (xdg_)
        for (auto& s : screens_) {
            s->xdg = zxdg_output_manager_v1_get_xdg_output(xdg_, s->wl);
            zxdg_output_v1_add_listener(s->xdg, &kXdgOutput, s.get());
        }
    wl_display_roundtrip(display_);
    if (!copy_ || !sources_) {
        error = "the compositor has no ext-image-copy-capture";
        return false;
    }
    if (!dmabuf_) {
        error = "the compositor has no linux-dmabuf (v3)";
        return false;
    }
    return true;
}

bool Capture::start(Screen& screen, bool cursor, bool ten_bit, int64_t interval_ns, std::string& error) {
    ten_bit_ = ten_bit;
    interval_ns_ = interval_ns;
    refresh_ns_ = screen.refresh_mhz > 0 ? int64_t(1000000000000LL / screen.refresh_mhz) : 16666667;
    ext_image_capture_source_v1* source = ext_output_image_capture_source_manager_v1_create_source(sources_, screen.wl);
    session_ = ext_image_copy_capture_manager_v1_create_session(
        copy_, source, cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(session_, &kSession, this);
    ext_image_capture_source_v1_destroy(source);
    while (!constraints_done_ && !stopped_)
        if (wl_display_dispatch(display_) < 0) {
            error = "the Wayland connection broke";
            return false;
        }
    if (stopped_) {
        error = "the compositor wouldn't copy the screen";
        return false;
    }
    if (device_ == 0 || formats_.empty()) {
        error = "the compositor doesn't copy the screen into GPU buffers (dmabuf)";
        return false;
    }
    drmDevice* dev = nullptr;
    if (drmGetDeviceFromDevId(device_, 0, &dev) != 0 || !dev) {
        error = "the compositor's GPU isn't one this process can see";
        return false;
    }
    if (dev->available_nodes & (1 << DRM_NODE_RENDER))
        render_node_ = dev->nodes[DRM_NODE_RENDER];
    else if (dev->available_nodes & (1 << DRM_NODE_PRIMARY))
        render_node_ = dev->nodes[DRM_NODE_PRIMARY];
    drmFreeDevice(&dev);
    gbm_fd_ = open(render_node_.c_str(), O_RDWR | O_CLOEXEC);
    if (gbm_fd_ < 0 || !(gbm_ = gbm_create_device(gbm_fd_))) {
        error = "can't open " + render_node_;
        return false;
    }
    return allocate(error);
}

bool Capture::allocate(std::string& error) {
    free_pool();
    reallocate_ = false;
    std::vector<const Format*> order;
    for (const Format& f : formats_)
        if (rank(f.fourcc, ten_bit_) != INT_MAX)
            order.push_back(&f);
    std::stable_sort(order.begin(), order.end(),
                     [this](const Format* a, const Format* b) { return rank(a->fourcc, ten_bit_) < rank(b->fourcc, ten_bit_); });
    // Three: the newest, the one being copied into, and one spare so a copy
    // never waits on the recorder.
    constexpr int kPool = 3;
    for (const Format* f : order) {
        bool ok = true;
        for (int i = 0; i < kPool && ok; i++) {
            Buffer b;
            std::vector<uint64_t> mods;
            for (uint64_t m : f->modifiers)
                if (m != DRM_FORMAT_MOD_INVALID)
                    mods.push_back(m);
            b.bo = mods.empty() ? gbm_bo_create(gbm_, uint32_t(width_), uint32_t(height_), f->fourcc, GBM_BO_USE_RENDERING)
                                : gbm_bo_create_with_modifiers2(gbm_, uint32_t(width_), uint32_t(height_), f->fourcc,
                                                                mods.data(), unsigned(mods.size()), GBM_BO_USE_RENDERING);
            if (!b.bo) {
                ok = false;
                break;
            }
            Dmabuf& d = b.dmabuf;
            d.planes = gbm_bo_get_plane_count(b.bo);
            d.fourcc = f->fourcc;
            d.modifier = mods.empty() ? DRM_FORMAT_MOD_INVALID : gbm_bo_get_modifier(b.bo);
            d.width = width_;
            d.height = height_;
            d.id = next_id_++;
            zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabuf_);
            for (int p = 0; p < d.planes; p++) {
                d.fd[p] = gbm_bo_get_fd_for_plane(b.bo, p);
                d.offset[p] = gbm_bo_get_offset(b.bo, p);
                d.stride[p] = gbm_bo_get_stride_for_plane(b.bo, p);
                const uint64_t mod = d.modifier == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_INVALID : d.modifier;
                zwp_linux_buffer_params_v1_add(params, d.fd[p], uint32_t(p), d.offset[p], d.stride[p], uint32_t(mod >> 32),
                                               uint32_t(mod & 0xffffffff));
            }
            b.wl = zwp_linux_buffer_params_v1_create_immed(params, width_, height_, f->fourcc, 0);
            zwp_linux_buffer_params_v1_destroy(params);
            pool_.push_back(b);
        }
        if (ok)
            return true;
        free_pool();
    }
    error = "no GPU buffer the compositor copies into could be made";
    return false;
}

void Capture::free_pool() {
    for (Buffer& b : pool_) {
        if (b.wl)
            wl_buffer_destroy(b.wl);
        for (int p = 0; p < b.dmabuf.planes; p++)
            if (b.dmabuf.fd[p] >= 0)
                close(b.dmabuf.fd[p]);
        if (b.bo)
            gbm_bo_destroy(b.bo);
    }
    pool_.clear();
    latest_ = pending_ = -1;
}

int Capture::fd() const {
    return wl_display_get_fd(display_);
}

bool Capture::flush() {
    return wl_display_flush(display_) >= 0 || errno == EAGAIN;
}

bool Capture::dispatch() {
    if (wl_display_prepare_read(display_) == 0) {
        if (wl_display_read_events(display_) < 0)
            return false;
    }
    return wl_display_dispatch_pending(display_) >= 0;
}

void Capture::request() {
    // A buffer that's neither the newest nor being copied into.
    int pick = -1;
    for (int i = 0; i < int(pool_.size()); i++)
        if (i != latest_ && i != pending_) {
            pick = i;
            break;
        }
    if (pick < 0)
        return;
    pending_ = pick;
    frame_ = ext_image_copy_capture_session_v1_create_frame(session_);
    ext_image_copy_capture_frame_v1_add_listener(frame_, &kFrame, this);
    ext_image_copy_capture_frame_v1_attach_buffer(frame_, pool_[pick].wl);
    ext_image_copy_capture_frame_v1_damage_buffer(frame_, 0, 0, INT32_MAX, INT32_MAX);
    ext_image_copy_capture_frame_v1_capture(frame_);
}

void Capture::pump(int64_t now_ns) {
    if (stopped_ || frame_)
        return;
    if (reallocate_ && constraints_done_) {
        std::string error;
        if (!allocate(error)) {
            std::fprintf(stderr, "atrium-record: %s\n", error.c_str());
            stopped_ = true;
            return;
        }
    }
    if (now_ns < next_request_ns_)
        return;
    request();
    next_request_ns_ = INT64_MAX;
}

} // namespace atrium::record
