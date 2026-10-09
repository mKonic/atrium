#include "handoff.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdlib>

namespace atrium {

namespace {

// The planes are dmabufs of their own: the framebuffer may go (its owner
// quits) while this still shows it.
struct ScanoutBuffer {
    wlr_buffer base;
    wlr_dmabuf_attributes dmabuf;
};

void scanout_destroy(wlr_buffer* buffer) {
    auto* b = reinterpret_cast<ScanoutBuffer*>(buffer);  // base is first
    wlr_buffer_finish(buffer);
    wlr_dmabuf_attributes_finish(&b->dmabuf);
    delete b;
}

bool scanout_get_dmabuf(wlr_buffer* buffer, wlr_dmabuf_attributes* out) {
    auto* b = reinterpret_cast<ScanoutBuffer*>(buffer);  // base is first
    *out = b->dmabuf;
    return true;
}

const wlr_buffer_impl kScanoutImpl = [] {
    wlr_buffer_impl impl{};
    impl.destroy = scanout_destroy;
    impl.get_dmabuf = scanout_get_dmabuf;
    return impl;
}();

// mHz, as wlroots works a mode's refresh out.
int refresh_mhz(const drmModeModeInfo& m) {
    int64_t mhz = (int64_t(m.clock) * 1000000LL / m.htotal + m.vtotal / 2) / m.vtotal;
    if (m.flags & DRM_MODE_FLAG_INTERLACE)
        mhz *= 2;
    if (m.flags & DRM_MODE_FLAG_DBLSCAN)
        mhz /= 2;
    if (m.vscan > 1)
        mhz /= m.vscan;
    return int(mhz);
}

uint32_t current_crtc(int fd, uint32_t connector_id) {
    drmModeConnector* conn = drmModeGetConnectorCurrent(fd, connector_id);
    if (!conn)
        return 0;
    uint32_t crtc = 0;
    if (conn->encoder_id)
        if (drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id)) {
            crtc = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    drmModeFreeConnector(conn);
    return crtc;
}

} // namespace

Scanout capture_scanout(wlr_output* output) {
    Scanout out;
    if (!wlr_output_is_drm(output))
        return out;
    const int fd = wlr_backend_get_drm_fd(output->backend);
    const uint32_t crtc_id = fd >= 0 ? current_crtc(fd, wlr_drm_connector_get_id(output)) : 0;
    drmModeCrtc* crtc = crtc_id ? drmModeGetCrtc(fd, crtc_id) : nullptr;
    if (!crtc)
        return out;
    const uint32_t fb_id = crtc->mode_valid ? crtc->buffer_id : 0;
    const drmModeModeInfo mode = crtc->mode;
    drmModeFreeCrtc(crtc);
    // Its handles only come to the DRM master, which atrium is by now.
    drmModeFB2* fb = fb_id ? drmModeGetFB2(fd, fb_id) : nullptr;
    if (!fb)
        return out;
    auto* b = new ScanoutBuffer{};
    for (int& plane : b->dmabuf.fd)
        plane = -1;
    b->dmabuf.width = int(fb->width);
    b->dmabuf.height = int(fb->height);
    b->dmabuf.format = fb->pixel_format;
    b->dmabuf.modifier = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : DRM_FORMAT_MOD_INVALID;
    bool ok = fb->handles[0] != 0;
    for (int i = 0; ok && i < 4 && fb->handles[i]; ++i) {
        ok = drmPrimeHandleToFD(fd, fb->handles[i], DRM_CLOEXEC, &b->dmabuf.fd[i]) == 0;
        b->dmabuf.offset[i] = fb->offsets[i];
        b->dmabuf.stride[i] = fb->pitches[i];
        b->dmabuf.n_planes = i + 1;
    }
    // GEM handles are this fd's, shared with wlroots: let go of each once.
    for (int i = 0; i < 4 && fb->handles[i]; ++i) {
        bool seen = false;
        for (int j = 0; j < i; ++j)
            seen = seen || fb->handles[j] == fb->handles[i];
        if (!seen)
            drmCloseBufferHandle(fd, fb->handles[i]);
    }
    drmModeFreeFB2(fb);
    if (!ok) {
        for (int i = 0; i < b->dmabuf.n_planes; ++i)
            if (b->dmabuf.fd[i] >= 0)
                close(b->dmabuf.fd[i]);
        delete b;
        return out;
    }
    wlr_buffer_init(&b->base, &kScanoutImpl, b->dmabuf.width, b->dmabuf.height);
    out.buffer = &b->base;
    const int mhz = refresh_mhz(mode);
    wlr_output_mode* m;
    wl_list_for_each(m, &output->modes, link)
        if (m->width == mode.hdisplay && m->height == mode.vdisplay && std::abs(m->refresh - mhz) <= 1) {
            out.mode = m;
            break;
        }
    return out;
}

} // namespace atrium
