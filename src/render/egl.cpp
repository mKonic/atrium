#include "render/egl.hpp"
#include "util/log.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <xf86drm.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace atrium::render {

bool has_extension(const char* list, const char* ext) {
    if (!list)
        return false;
    const size_t len = std::strlen(ext);
    for (const char* p = list; *p;) {
        while (*p == ' ')
            ++p;
        const size_t n = std::strcspn(p, " ");
        if (n == len && std::strncmp(p, ext, n) == 0)
            return true;
        p += n;
    }
    return false;
}

namespace {

bool env_true(const char* name) {
    const char* v = std::getenv(name);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}

void egl_log(EGLenum error, const char* command, EGLint type, EGLLabelKHR, EGLLabelKHR, const char* msg) {
    const Log level =
        type == EGL_DEBUG_MSG_CRITICAL_KHR || type == EGL_DEBUG_MSG_ERROR_KHR || type == EGL_DEBUG_MSG_WARN_KHR
            ? Log::Error
            : Log::Info;
    alog(level, "[EGL] %s: 0x%x: %s", command, error, msg);
}

bool device_has_name(const drmDevice* device, const char* name) {
    for (int i = 0; i < DRM_NODE_MAX; ++i)
        if ((device->available_nodes & (1 << i)) && std::strcmp(device->nodes[i], name) == 0)
            return true;
    return false;
}

// The render node of the device with node `name` (its primary node on
// split display/render hardware).
std::string render_name_for(const char* name) {
    int n = drmGetDevices2(0, nullptr, 0);
    if (n <= 0)
        return {};
    std::vector<drmDevice*> devices(n);
    n = drmGetDevices2(0, devices.data(), n);
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (out.empty() && device_has_name(devices[i], name)) {
            if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER))
                out = devices[i]->nodes[DRM_NODE_RENDER];
            else
                out = devices[i]->nodes[DRM_NODE_PRIMARY];
        }
    }
    for (int i = 0; i < n; ++i)
        drmFreeDevice(&devices[i]);
    return out;
}

int open_render_node(int drm_fd) {
    char* name = drmGetRenderDeviceNameFromFd(drm_fd);
    if (!name)
        name = drmGetPrimaryDeviceNameFromFd(drm_fd);
    if (!name)
        return -1;
    int fd = open(name, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        alog_errno(Log::Error, "Failed to open DRM node %s", name);
    free(name);
    return fd;
}

} // namespace

std::unique_ptr<Egl> Egl::create(int drm_fd, bool software_ok) {
    std::unique_ptr<Egl> egl(new Egl());
    if (!egl->load_client_extensions())
        return nullptr;
    const bool allow_software = software_ok || drm_fd < 0;

    if (egl->exts.EXT_platform_device) {
        EGLDeviceEXT device = egl->device_for(drm_fd);
        if (device != EGL_NO_DEVICE_EXT) {
            if (egl->init(EGL_PLATFORM_DEVICE_EXT, device, allow_software))
                return egl;
            return nullptr;
        }
    }
    if (egl->exts.KHR_platform_gbm && drm_fd >= 0) {
        int gbm_fd = open_render_node(drm_fd);
        if (gbm_fd < 0)
            return nullptr;
        egl->gbm = gbm_create_device(gbm_fd);
        if (!egl->gbm) {
            close(gbm_fd);
            return nullptr;
        }
        if (egl->init(EGL_PLATFORM_GBM_KHR, egl->gbm, allow_software))
            return egl;
    }
    alog(Log::Error, "Couldn't initialize EGL");
    return nullptr;
}

Egl::~Egl() {
    wlr_drm_format_set_finish(&render_formats_);
    wlr_drm_format_set_finish(&texture_formats_);
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        if (exts.KHR_display_reference)
            eglTerminate(display);
    }
    eglReleaseThread();
    if (gbm) {
        int fd = gbm_device_get_fd(gbm);
        gbm_device_destroy(gbm);
        close(fd);
    }
}

bool Egl::load_client_extensions() {
    const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client) {
        alog(Log::Error, "EGL client extensions unsupported");
        return false;
    }
    if (!has_extension(client, "EGL_EXT_platform_base") ||
        !load_proc(procs.eglGetPlatformDisplayEXT, "eglGetPlatformDisplayEXT")) {
        alog(Log::Error, "EGL_EXT_platform_base unsupported");
        return false;
    }
    exts.KHR_platform_gbm = has_extension(client, "EGL_KHR_platform_gbm");
    exts.EXT_platform_device = has_extension(client, "EGL_EXT_platform_device");
    exts.KHR_display_reference = has_extension(client, "EGL_KHR_display_reference");
    if (has_extension(client, "EGL_EXT_device_base") || has_extension(client, "EGL_EXT_device_enumeration"))
        load_proc(procs.eglQueryDevicesEXT, "eglQueryDevicesEXT");
    if (has_extension(client, "EGL_EXT_device_base") || has_extension(client, "EGL_EXT_device_query")) {
        exts.EXT_device_query = load_proc(procs.eglQueryDeviceStringEXT, "eglQueryDeviceStringEXT") &&
                                load_proc(procs.eglQueryDisplayAttribEXT, "eglQueryDisplayAttribEXT");
    }
    if (has_extension(client, "EGL_KHR_debug") &&
        load_proc(procs.eglDebugMessageControlKHR, "eglDebugMessageControlKHR")) {
        static const EGLAttrib attribs[] = {
            EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE, EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE,
            EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE,
        };
        procs.eglDebugMessageControlKHR(egl_log, attribs);
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        alog(Log::Error, "Couldn't bind the OpenGL ES API");
        return false;
    }
    return true;
}

EGLDeviceEXT Egl::device_for(int drm_fd) {
    if (!procs.eglQueryDevicesEXT || !exts.EXT_device_query)
        return EGL_NO_DEVICE_EXT;
    EGLint count = 0;
    if (!procs.eglQueryDevicesEXT(0, nullptr, &count) || count <= 0)
        return EGL_NO_DEVICE_EXT;
    std::vector<EGLDeviceEXT> devices(count);
    if (!procs.eglQueryDevicesEXT(count, devices.data(), &count))
        return EGL_NO_DEVICE_EXT;

    drmDevice* wanted = nullptr;
    if (drm_fd >= 0 && drmGetDevice(drm_fd, &wanted) < 0)
        return EGL_NO_DEVICE_EXT;
    EGLDeviceEXT found = EGL_NO_DEVICE_EXT;
    for (EGLint i = 0; i < count && found == EGL_NO_DEVICE_EXT; ++i) {
        const char* dexts = procs.eglQueryDeviceStringEXT(devices[i], EGL_EXTENSIONS);
        if (!dexts)
            continue;
        const char* name = nullptr;
        if (has_extension(dexts, "EGL_EXT_device_drm"))
            name = procs.eglQueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);
        const bool software = has_extension(dexts, "EGL_MESA_device_software");
        if (wanted ? (name && device_has_name(wanted, name)) : software)
            found = devices[i];
    }
    drmFreeDevice(&wanted);
    return found;
}

bool Egl::init(EGLenum platform, void* native, bool allow_software) {
    std::vector<EGLint> dattribs;
    if (exts.KHR_display_reference)
        dattribs.insert(dattribs.end(), {EGL_TRACK_REFERENCES_KHR, EGL_TRUE});
    dattribs.push_back(EGL_NONE);
    EGLDisplay d = procs.eglGetPlatformDisplayEXT(platform, native, dattribs.data());
    if (d == EGL_NO_DISPLAY) {
        alog(Log::Error, "Couldn't create an EGL display");
        return false;
    }
    if (!init_display(d, allow_software)) {
        if (exts.KHR_display_reference)
            eglTerminate(d);
        display = EGL_NO_DISPLAY;
        return false;
    }

    // GLES 3.2, else the newest 3.x there is: the shaders are GLSL ES 3.00.
    for (int minor : {2, 1, 0}) {
        std::vector<EGLint> attribs = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor};
        if (exts.IMG_context_priority)
            attribs.insert(attribs.end(), {EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG});
        if (exts.EXT_create_context_robustness)
            attribs.insert(attribs.end(),
                           {EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT, EGL_LOSE_CONTEXT_ON_RESET_EXT});
        attribs.push_back(EGL_NONE);
        context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs.data());
        if (context != EGL_NO_CONTEXT) {
            gl_major = 3;
            gl_minor = minor;
            break;
        }
    }
    if (context == EGL_NO_CONTEXT) {
        alog(Log::Error, "Couldn't create a GLES 3 context");
        return false;
    }
    alog(Log::Info, "Created a GLES %d.%d context", gl_major, gl_minor);
    if (exts.IMG_context_priority) {
        EGLint priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
        eglQueryContext(display, context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &priority);
        alog(Log::Debug, "Context priority %s", priority == EGL_CONTEXT_PRIORITY_HIGH_IMG ? "high" : "default");
    }
    return true;
}

bool Egl::init_display(EGLDisplay d, bool allow_software) {
    display = d;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        alog(Log::Error, "Couldn't initialize EGL");
        return false;
    }
    const char* dexts = eglQueryString(display, EGL_EXTENSIONS);
    if (!dexts)
        return false;

    if (has_extension(dexts, "EGL_KHR_image_base"))
        exts.KHR_image_base = load_proc(procs.eglCreateImageKHR, "eglCreateImageKHR") &&
                              load_proc(procs.eglDestroyImageKHR, "eglDestroyImageKHR");
    exts.EXT_image_dma_buf_import = has_extension(dexts, "EGL_EXT_image_dma_buf_import");
    if (has_extension(dexts, "EGL_EXT_image_dma_buf_import_modifiers"))
        exts.EXT_image_dma_buf_import_modifiers =
            load_proc(procs.eglQueryDmaBufFormatsEXT, "eglQueryDmaBufFormatsEXT") &&
            load_proc(procs.eglQueryDmaBufModifiersEXT, "eglQueryDmaBufModifiersEXT");
    exts.EXT_create_context_robustness = has_extension(dexts, "EGL_EXT_create_context_robustness");

    const char* device_exts = nullptr;
    if (exts.EXT_device_query) {
        EGLAttrib attrib = 0;
        if (!procs.eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &attrib))
            return false;
        device = reinterpret_cast<EGLDeviceEXT>(attrib);
        device_exts = procs.eglQueryDeviceStringEXT(device, EGL_EXTENSIONS);
        if (!device_exts)
            return false;
        exts.EXT_device_drm = has_extension(device_exts, "EGL_EXT_device_drm");
        exts.EXT_device_drm_render_node = has_extension(device_exts, "EGL_EXT_device_drm_render_node");
        if (has_extension(device_exts, "EGL_MESA_device_software")) {
            if (!allow_software && !env_true("WLR_RENDERER_ALLOW_SOFTWARE")) {
                alog(Log::Error, "Only software rendering is available; set "
                                   "WLR_RENDERER_ALLOW_SOFTWARE=1 to use it");
                return false;
            }
            alog(Log::Info, "Using software rendering");
        }
    }

    if (!has_extension(dexts, "EGL_KHR_no_config_context") &&
        !has_extension(dexts, "EGL_MESA_configless_context")) {
        alog(Log::Error, "EGL_KHR_no_config_context unsupported");
        return false;
    }
    if (!has_extension(dexts, "EGL_KHR_surfaceless_context")) {
        alog(Log::Error, "EGL_KHR_surfaceless_context unsupported");
        return false;
    }
    if (has_extension(dexts, "EGL_KHR_fence_sync") && has_extension(dexts, "EGL_ANDROID_native_fence_sync")) {
        load_proc(procs.eglCreateSyncKHR, "eglCreateSyncKHR");
        load_proc(procs.eglDestroySyncKHR, "eglDestroySyncKHR");
        load_proc(procs.eglDupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID");
    }
    if (has_extension(dexts, "EGL_KHR_wait_sync"))
        load_proc(procs.eglWaitSyncKHR, "eglWaitSyncKHR");
    exts.IMG_context_priority = has_extension(dexts, "EGL_IMG_context_priority");

    alog(Log::Info, "EGL %d.%d, vendor %s", major, minor, eglQueryString(display, EGL_VENDOR));
    alog(Log::Debug, "EGL display extensions: %s", dexts);
    if (device_exts)
        alog(Log::Debug, "EGL device extensions: %s", device_exts);

    init_dmabuf_formats();
    return true;
}

void Egl::init_dmabuf_formats() {
    if (!exts.EXT_image_dma_buf_import)
        return;
    const bool no_modifiers = env_true("WLR_EGL_NO_MODIFIERS");

    std::vector<EGLint> formats;
    if (!exts.EXT_image_dma_buf_import_modifiers) {
        // Can't ask: the two formats every driver takes.
        formats = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
    } else {
        EGLint n = 0;
        if (!procs.eglQueryDmaBufFormatsEXT(display, 0, nullptr, &n))
            return;
        formats.resize(n);
        if (!procs.eglQueryDmaBufFormatsEXT(display, n, formats.data(), &n))
            return;
        formats.resize(n);
    }

    for (EGLint fmt : formats) {
        std::vector<uint64_t> modifiers;
        std::vector<EGLBoolean> external;
        if (exts.EXT_image_dma_buf_import_modifiers && !no_modifiers) {
            EGLint n = 0;
            if (!procs.eglQueryDmaBufModifiersEXT(display, fmt, 0, nullptr, nullptr, &n))
                continue;
            modifiers.resize(n);
            external.resize(n);
            if (n > 0 && !procs.eglQueryDmaBufModifiersEXT(display, fmt, n, modifiers.data(), external.data(), &n))
                continue;
        }
        has_modifiers_ = has_modifiers_ || !modifiers.empty();
        bool all_external = true;
        for (size_t i = 0; i < modifiers.size(); ++i) {
            wlr_drm_format_set_add(&texture_formats_, fmt, modifiers[i]);
            if (!external[i]) {
                wlr_drm_format_set_add(&render_formats_, fmt, modifiers[i]);
                all_external = false;
            }
        }
        // Implicit modifiers always work; renderable if anything is.
        wlr_drm_format_set_add(&texture_formats_, fmt, DRM_FORMAT_MOD_INVALID);
        if (modifiers.empty() || !all_external)
            wlr_drm_format_set_add(&render_formats_, fmt, DRM_FORMAT_MOD_INVALID);
        if (modifiers.empty()) {
            wlr_drm_format_set_add(&texture_formats_, fmt, DRM_FORMAT_MOD_LINEAR);
            wlr_drm_format_set_add(&render_formats_, fmt, DRM_FORMAT_MOD_LINEAR);
        }
    }
}

bool Egl::make_current() {
    if (eglGetCurrentContext() == context)
        return true;
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        alog(Log::Error, "eglMakeCurrent failed");
        return false;
    }
    return true;
}

void Egl::unset_current() {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

EGLImageKHR Egl::import_dmabuf(const wlr_dmabuf_attributes& a, bool* external_only) {
    if (!exts.KHR_image_base || !exts.EXT_image_dma_buf_import)
        return EGL_NO_IMAGE_KHR;
    if (a.modifier != DRM_FORMAT_MOD_INVALID && a.modifier != DRM_FORMAT_MOD_LINEAR && !has_modifiers_)
        return EGL_NO_IMAGE_KHR;

    static constexpr EGLint names[4][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
    };
    std::vector<EGLint> attribs = {EGL_WIDTH, a.width, EGL_HEIGHT, a.height,
                                   EGL_LINUX_DRM_FOURCC_EXT, EGLint(a.format)};
    for (int i = 0; i < a.n_planes && i < 4; ++i) {
        attribs.insert(attribs.end(), {names[i][0], a.fd[i], names[i][1], EGLint(a.offset[i]), names[i][2],
                                       EGLint(a.stride[i])});
        if (has_modifiers_ && a.modifier != DRM_FORMAT_MOD_INVALID)
            attribs.insert(attribs.end(), {names[i][3], EGLint(a.modifier & 0xFFFFFFFF), names[i][4],
                                           EGLint(a.modifier >> 32)});
    }
    // Clients don't expect sampling to trash their buffers.
    attribs.insert(attribs.end(), {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE});

    EGLImageKHR image =
        procs.eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        alog(Log::Error, "eglCreateImageKHR failed (format 0x%08x, modifier 0x%016llx)", a.format,
                (unsigned long long)a.modifier);
        return EGL_NO_IMAGE_KHR;
    }
    *external_only = !wlr_drm_format_set_has(&render_formats_, a.format, a.modifier);
    return image;
}

void Egl::destroy_image(EGLImageKHR image) {
    if (image != EGL_NO_IMAGE_KHR && exts.KHR_image_base)
        procs.eglDestroyImageKHR(display, image);
}

int Egl::dup_drm_fd() {
    if (device != EGL_NO_DEVICE_EXT && (exts.EXT_device_drm || exts.EXT_device_drm_render_node)) {
        std::string name;
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
        if (exts.EXT_device_drm_render_node)
            if (const char* n = procs.eglQueryDeviceStringEXT(device, EGL_DRM_RENDER_NODE_FILE_EXT))
                name = n;
#endif
        if (name.empty())
            if (const char* primary = procs.eglQueryDeviceStringEXT(device, EGL_DRM_DEVICE_FILE_EXT))
                name = render_name_for(primary);
        if (!name.empty()) {
            int fd = open(name.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0)
                return fd;
            alog_errno(Log::Error, "Failed to open %s", name.c_str());
        }
    }
    if (gbm)
        return fcntl(gbm_device_get_fd(gbm), F_DUPFD_CLOEXEC, 0);
    return -1;
}

EGLSyncKHR Egl::create_sync(int fence_fd) {
    if (!procs.eglCreateSyncKHR)
        return EGL_NO_SYNC_KHR;
    EGLint attribs[3] = {EGL_NONE};
    int dup_fd = -1;
    if (fence_fd >= 0) {
        dup_fd = fcntl(fence_fd, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0)
            return EGL_NO_SYNC_KHR;
        attribs[0] = EGL_SYNC_NATIVE_FENCE_FD_ANDROID;
        attribs[1] = dup_fd;
        attribs[2] = EGL_NONE;
    }
    EGLSyncKHR sync = procs.eglCreateSyncKHR(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
    if (sync == EGL_NO_SYNC_KHR && dup_fd >= 0)
        close(dup_fd);  // EGL owns it only on success
    return sync;
}

void Egl::destroy_sync(EGLSyncKHR sync) {
    if (sync != EGL_NO_SYNC_KHR && procs.eglDestroySyncKHR)
        procs.eglDestroySyncKHR(display, sync);
}

int Egl::dup_fence_fd(EGLSyncKHR sync) {
    if (!procs.eglDupNativeFenceFDANDROID)
        return -1;
    int fd = procs.eglDupNativeFenceFDANDROID(display, sync);
    return fd == EGL_NO_NATIVE_FENCE_FD_ANDROID ? -1 : fd;
}

bool Egl::wait_sync(EGLSyncKHR sync) {
    return procs.eglWaitSyncKHR && procs.eglWaitSyncKHR(display, sync, 0) == EGL_TRUE;
}

} // namespace atrium::render
