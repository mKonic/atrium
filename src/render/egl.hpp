#pragma once
// atrium's EGL: a display on the GPU the backend drives, one GLES 3 context,
// dmabuf import and native fences. Ported from wlroots' render/egl.c (MIT;
// Copyright (c) 2017, 2018 Drew DeVault, 2014 Jari Vetoniemi), by way of
// scenefx.

#include "render/gl.hpp"

#include <memory>

struct gbm_device;

namespace atrium::render {

class Egl {
public:
    // A display for the DRM device behind `drm_fd` (any render node when
    // it's -1 and software is allowed), or null.
    static std::unique_ptr<Egl> create(int drm_fd);
    ~Egl();
    Egl(const Egl&) = delete;
    Egl& operator=(const Egl&) = delete;

    // Our context current, unless it already is.
    bool make_current();
    void unset_current();

    // An image of a dmabuf; `external_only` says it can only be sampled
    // through GL_TEXTURE_EXTERNAL_OES (never rendered into).
    EGLImageKHR import_dmabuf(const wlr_dmabuf_attributes& attribs, bool* external_only);
    void destroy_image(EGLImageKHR image);

    // A render node for the device, for the caller to own (-1 if none).
    int dup_drm_fd();

    // Native fences (EGL_ANDROID_native_fence_sync): -1 makes one signalled
    // when the GPU gets past this point.
    EGLSyncKHR create_sync(int fence_fd);
    void destroy_sync(EGLSyncKHR sync);
    int dup_fence_fd(EGLSyncKHR sync);
    bool wait_sync(EGLSyncKHR sync);
    bool has_fences() const { return procs.eglDupNativeFenceFDANDROID && procs.eglWaitSyncKHR; }

    const wlr_drm_format_set* texture_formats() const { return &texture_formats_; }
    const wlr_drm_format_set* render_formats() const { return &render_formats_; }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    gbm_device* gbm = nullptr;
    int gl_major = 0, gl_minor = 0;

    struct {
        bool KHR_image_base = false;
        bool EXT_image_dma_buf_import = false;
        bool EXT_image_dma_buf_import_modifiers = false;
        bool IMG_context_priority = false;
        bool EXT_create_context_robustness = false;
        bool EXT_device_drm = false;
        bool EXT_device_drm_render_node = false;
        bool EXT_device_query = false;
        bool KHR_platform_gbm = false;
        bool EXT_platform_device = false;
        bool KHR_display_reference = false;
    } exts;

    struct {
        PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = nullptr;
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
        PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT = nullptr;
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT = nullptr;
        PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = nullptr;
        PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT = nullptr;
        PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT = nullptr;
        PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = nullptr;
        PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = nullptr;
        PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = nullptr;
        PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID = nullptr;
        PFNEGLWAITSYNCKHRPROC eglWaitSyncKHR = nullptr;
    } procs;

private:
    Egl() = default;
    bool load_client_extensions();
    bool init(EGLenum platform, void* native, bool allow_software);
    bool init_display(EGLDisplay display, bool allow_software);
    EGLDeviceEXT device_for(int drm_fd);
    void init_dmabuf_formats();

    bool has_modifiers_ = false;
    wlr_drm_format_set texture_formats_{};
    wlr_drm_format_set render_formats_{};
};

} // namespace atrium::render
