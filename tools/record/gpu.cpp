#include "gpu.hpp"

#include "record_core.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <xf86drm.h>

#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef GL_R16_EXT
#define GL_R16_EXT 0x822A
#define GL_RG16_EXT 0x822C
#endif

namespace atrium::record {

namespace {

// The vertex shader covers the viewport and works out where in the captured
// buffer each corner of the upright, cropped picture is.
const char* kVertex = R"(#version 300 es
uniform mat2 map;      // upright (centred) → buffer (centred)
uniform vec4 crop;     // the upright picture's part: x, y, width, height (0-1)
out vec2 uv;
void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 upright = crop.xy + corner * crop.zw;
    uv = map * (upright - 0.5) + 0.5;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

// RGB in, one plane's YUV out. atrium hands every copy over as SDR (an HDR
// screen's buffer converted back, 1.0 = SDR white, night light already taken
// out), so the paths are gpu-screen-recorder's for an SDR source: as it is,
// or into HDR video with SDR white at BT.2408's reference (203 nits). On an
// SDR screen night light's tint, drawn into the frame, is divided out in
// linear light first.
const char* kFragment = R"(
precision highp float;
in vec2 uv;
uniform SAMPLER tex;
uniform mat4 to_yuv;
uniform float gain;
uniform int pass;          // 0: Y, 1: UV
uniform int mode;          // 0 SDR, 1 SDR into HDR (PQ BT.2020)
uniform vec3 night;
uniform float scale;
out vec4 color;

const float M1 = 0.1593017578125;
const float M2 = 78.84375;
const float C1 = 0.8359375;
const float C2 = 18.8515625;
const float C3 = 18.6875;
const mat3 BT709_TO_BT2020 = mat3(0.627404, 0.069097, 0.016391,
                                  0.329283, 0.919540, 0.088013,
                                  0.043313, 0.011362, 0.895595);

vec3 linear_to_pq(vec3 l) {
    vec3 p = pow(clamp(l, vec3(0.0), vec3(1.0)), vec3(M1));
    return pow((C1 + C2 * p) / (1.0 + C3 * p), vec3(M2));
}

void main() {
    vec3 c = texture(tex, uv).rgb;
    if (mode == 0) {
        if (night != vec3(1.0))
            c = pow(clamp(pow(max(c, vec3(0.0)), vec3(2.2)) / night, 0.0, 1.0), vec3(1.0 / 2.2));
    } else {
        vec3 l = pow(max(c, vec3(0.0)), vec3(2.2)) / night;
        c = linear_to_pq(max(BT709_TO_BT2020 * l * scale, vec3(0.0)));
    }
    vec4 yuv = to_yuv * vec4(c, 1.0);
    if (pass == 0)
        color = vec4(yuv.x * gain, 0.0, 0.0, 1.0);
    else
        color = vec4(yuv.y, yuv.z, 0.0, 1.0);
}
)";

GLuint compile(GLenum type, const std::string& src, std::string& error) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = std::string("shader: ") + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void GL_APIENTRY gl_debug(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*) {
    std::fprintf(stderr, "atrium-record: gl %s: %s\n", type == GL_DEBUG_TYPE_ERROR_KHR ? "error" : "debug", message);
}

} // namespace

Gpu::~Gpu() {
    if (display_ == EGL_NO_DISPLAY)
        return;
    for (auto& [id, im] : imported_) {
        glDeleteTextures(1, &im.texture);
        egl_destroy_image(display_, im.image);
    }
    for (Program& p : programs_)
        if (p.id)
            glDeleteProgram(p.id);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_ != EGL_NO_CONTEXT)
        eglDestroyContext(display_, context_);
    eglTerminate(display_);
}

bool Gpu::has_extension(const char* name) const {
    const std::string n = std::string(" ") + name + " ";
    return (" " + egl_extensions_ + " ").find(n) != std::string::npos;
}

bool Gpu::open(const std::string& render_node, bool debug, std::string& error) {
    node_ = render_node;
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto query_devices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto query_device_string =
        reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (!get_platform_display) {
        error = "EGL has no eglGetPlatformDisplayEXT";
        return false;
    }
    // The EGL device of that GPU (gsr's EGL_EXT_device path): surfaceless,
    // no window system in between.
    if (query_devices && query_device_string) {
        EGLDeviceEXT devices[32];
        EGLint n = 0;
        if (query_devices(32, devices, &n))
            for (EGLint i = 0; i < n && display_ == EGL_NO_DISPLAY; i++) {
                const char* exts = query_device_string(devices[i], EGL_EXTENSIONS);
                if (!exts || !std::strstr(exts, "EGL_EXT_device_drm"))
                    continue;
                const char* node = nullptr;
                if (std::strstr(exts, "EGL_EXT_device_drm_render_node"))
                    node = query_device_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
                bool same = node && render_node == node;
                if (!same) {
                    // Only the primary node named: compare the GPUs.
                    const char* primary = query_device_string(devices[i], EGL_DRM_DEVICE_FILE_EXT);
                    drmDevice* a = nullptr;
                    int fd = primary ? ::open(primary, O_RDONLY | O_CLOEXEC) : -1;
                    if (fd >= 0 && drmGetDevice2(fd, 0, &a) == 0 && a) {
                        same = (a->available_nodes & (1 << DRM_NODE_RENDER)) && render_node == a->nodes[DRM_NODE_RENDER];
                        drmFreeDevice(&a);
                    }
                    if (fd >= 0)
                        close(fd);
                }
                if (same)
                    display_ = get_platform_display(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
            }
    }
    if (display_ == EGL_NO_DISPLAY) {
        int fd = ::open(render_node.c_str(), O_RDWR | O_CLOEXEC);
        gbm_device* gbm = fd >= 0 ? gbm_create_device(fd) : nullptr;
        if (gbm)
            display_ = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    }
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        error = "no EGL display on " + render_node;
        display_ = EGL_NO_DISPLAY;
        return false;
    }
    egl_extensions_ = eglQueryString(display_, EGL_EXTENSIONS);
    if (!has_extension("EGL_EXT_image_dma_buf_import") || !has_extension("EGL_KHR_surfaceless_context")) {
        error = "EGL can't take dmabufs or work without a surface here";
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig config = EGL_NO_CONFIG_KHR;
    if (!has_extension("EGL_KHR_no_config_context")) {
        const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
        EGLint n = 0;
        eglChooseConfig(display_, want, &config, 1, &n);
    }
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_OPENGL_DEBUG, debug ? EGL_TRUE : EGL_FALSE, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, attrs);
    if (context_ == EGL_NO_CONTEXT || !eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
        error = "no OpenGL ES 3 context on " + render_node;
        return false;
    }
    egl_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    image_target_texture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!egl_create_image || !egl_destroy_image || !image_target_texture) {
        error = "EGL has no EGLImage functions";
        return false;
    }
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    vendor_ = vendor ? vendor : "";
    nvidia_ = vendor_.find("NVIDIA") != std::string::npos;
    if (renderer && std::strstr(renderer, "llvmpipe")) {
        error = "the GPU is llvmpipe (software rendering)";
        return false;
    }
    if (debug) {
        auto callback = reinterpret_cast<PFNGLDEBUGMESSAGECALLBACKKHRPROC>(eglGetProcAddress("glDebugMessageCallbackKHR"));
        if (callback) {
            glEnable(GL_DEBUG_OUTPUT_KHR);
            callback(gl_debug, nullptr);
        }
    }
    glGenVertexArrays(1, &vao_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    return true;
}

bool Gpu::build(Program& p, bool external, std::string& error) {
    std::string frag = "#version 300 es\n";
    if (external)
        frag += "#extension GL_OES_EGL_image_external_essl3 : require\n#define SAMPLER samplerExternalOES\n";
    else
        frag += "#define SAMPLER sampler2D\n";
    frag += kFragment;
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, frag, error) : 0;
    if (!fs) {
        if (vs)
            glDeleteShader(vs);
        return false;
    }
    p.id = glCreateProgram();
    glAttachShader(p.id, vs);
    glAttachShader(p.id, fs);
    glLinkProgram(p.id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
        error = std::string("shader: ") + log;
        return false;
    }
    p.to_yuv = glGetUniformLocation(p.id, "to_yuv");
    p.gain = glGetUniformLocation(p.id, "gain");
    p.pass = glGetUniformLocation(p.id, "pass");
    p.mode = glGetUniformLocation(p.id, "mode");
    p.night = glGetUniformLocation(p.id, "night");
    p.scale = glGetUniformLocation(p.id, "scale");
    p.map = glGetUniformLocation(p.id, "map");
    p.offset = glGetUniformLocation(p.id, "crop");
    p.tex = glGetUniformLocation(p.id, "tex");
    return true;
}

bool Gpu::prepare(const ColorSetup& setup, std::string& error) {
    setup_ = setup;
    if (!build(programs_[0], false, error))
        return false;
    // External textures only where the driver has them (some modifiers are
    // external-only).
    std::string ignored;
    build(programs_[1], true, ignored);
    return true;
}

EGLImage Gpu::create_image(const Dmabuf& f, bool modifiers) {
    std::vector<EGLint> a = {EGL_WIDTH, f.width, EGL_HEIGHT, f.height, EGL_LINUX_DRM_FOURCC_EXT, EGLint(f.fourcc)};
    static const EGLint fd_keys[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                                      EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint off_keys[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                       EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint pitch_keys[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                         EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint lo_keys[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                      EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint hi_keys[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                      EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    for (int p = 0; p < f.planes; p++) {
        a.insert(a.end(), {fd_keys[p], f.fd[p], off_keys[p], EGLint(f.offset[p]), pitch_keys[p], EGLint(f.stride[p])});
        if (modifiers && f.modifier != DRM_FORMAT_MOD_INVALID)
            a.insert(a.end(), {lo_keys[p], EGLint(f.modifier & 0xffffffff), hi_keys[p], EGLint(f.modifier >> 32)});
    }
    a.insert(a.end(), {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE});
    return egl_create_image(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a.data());
}

Gpu::Imported* Gpu::import(const Dmabuf& frame) {
    if (auto it = imported_.find(frame.id); it != imported_.end()) {
        it->second.used = ++use_count_;
        return &it->second;
    }
    // The capture's pool is three buffers; a few more are kept across a
    // reallocation, the oldest dropped.
    while (imported_.size() >= 6) {
        auto oldest = std::min_element(imported_.begin(), imported_.end(),
                                       [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        glDeleteTextures(1, &oldest->second.texture);
        egl_destroy_image(display_, oldest->second.image);
        imported_.erase(oldest);
    }
    Imported im;
    // With modifiers, then (gsr's fallback) without.
    im.image = create_image(frame, true);
    if (im.image == EGL_NO_IMAGE_KHR)
        im.image = create_image(frame, false);
    if (im.image == EGL_NO_IMAGE_KHR) {
        std::fprintf(stderr, "atrium-record: the captured buffer couldn't be imported (EGL error 0x%x)\n", eglGetError());
        return nullptr;
    }
    glGenTextures(1, &im.texture);
    while (glGetError() != GL_NO_ERROR) {
    }
    glBindTexture(GL_TEXTURE_2D, im.texture);
    image_target_texture(GL_TEXTURE_2D, im.image);
    if (glGetError() != GL_NO_ERROR && programs_[1].id) {
        // An external-only layout.
        glDeleteTextures(1, &im.texture);
        glGenTextures(1, &im.texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, im.texture);
        image_target_texture(GL_TEXTURE_EXTERNAL_OES, im.image);
        im.external = true;
    }
    const GLenum target = im.external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    im.used = ++use_count_;
    return &(imported_[frame.id] = im);
}

bool Gpu::make_plane(GLenum format, int width, int height, GLuint& texture, GLuint& fbo) {
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

bool Gpu::import_plane(int fd, uint32_t offset, uint32_t stride, uint64_t modifier, uint32_t fourcc, int width, int height,
                       GLuint& texture, GLuint& fbo, EGLImage& image) {
    Dmabuf d;
    d.fd[0] = fd;
    d.offset[0] = offset;
    d.stride[0] = stride;
    d.planes = 1;
    d.fourcc = fourcc;
    d.modifier = modifier;
    d.width = width;
    d.height = height;
    image = create_image(d, true);
    if (image == EGL_NO_IMAGE_KHR)
        image = create_image(d, false);
    if (image == EGL_NO_IMAGE_KHR)
        return false;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    image_target_texture(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void Gpu::clear(const Planes& target) {
    const float* m = rgb_to_yuv(setup_.hdr_video, setup_.full_range);
    // Black: the matrix's offsets.
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo[0]);
    glClearColor(m[12] * luma_gain(setup_.ten_bit), 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo[1]);
    glClearColor(0.5f, 0.5f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Gpu::draw(const Dmabuf& frame, int32_t transform, const int crop_px[4], const ScreenColor& color,
               const Planes& target, int video_width, int video_height) {
    Imported* im = import(frame);
    if (!im)
        return false;
    Program& p = programs_[im->external ? 1 : 0];
    if (!p.id)
        return false;

    // The upright picture's size, and the part of it recorded.
    const bool sideways = transform & 1;
    const int upright_w = sideways ? frame.height : frame.width;
    const int upright_h = sideways ? frame.width : frame.height;
    float crop[4] = {0, 0, 1, 1};
    int crop_w = upright_w, crop_h = upright_h;
    if (crop_px && crop_px[2] > 0 && crop_px[3] > 0) {
        const int x = std::clamp(crop_px[0], 0, upright_w - 1), y = std::clamp(crop_px[1], 0, upright_h - 1);
        crop_w = std::clamp(crop_px[2], 1, upright_w - x);
        crop_h = std::clamp(crop_px[3], 1, upright_h - y);
        crop[0] = float(x) / float(upright_w);
        crop[1] = float(y) / float(upright_h);
        crop[2] = float(crop_w) / float(upright_w);
        crop[3] = float(crop_h) / float(upright_h);
    }
    // Fitted into the video, centred (a screen that changed size keeps its
    // shape, as gsr's).
    const Size fitted = fit({crop_w, crop_h}, {video_width, video_height});
    const int x0 = (video_width - fitted.width) / 2, y0 = (video_height - fitted.height) / 2;
    if (fitted.width != target.width[0] || fitted.height != target.height[0])
        clear(target);

    glUseProgram(p.id);
    glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(im->external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, im->texture);
    glUniform1i(p.tex, 0);
    glUniformMatrix4fv(p.to_yuv, 1, GL_FALSE, rgb_to_yuv(setup_.hdr_video, setup_.full_range));
    glUniform1f(p.gain, luma_gain(setup_.ten_bit));
    float map[4];
    transform_map(transform, map);
    glUniformMatrix2fv(p.map, 1, GL_FALSE, map);
    glUniform4fv(p.offset, 1, crop);
    const bool night_on = color.night[0] != 1 || color.night[1] != 1 || color.night[2] != 1;
    glUniform3f(p.night, night_on ? std::max(color.night[0], 0.01f) : 1.0f, night_on ? std::max(color.night[1], 0.01f) : 1.0f,
                night_on ? std::max(color.night[2], 0.01f) : 1.0f);
    // gsr's kms.c for an SDR source: SDR white at the BT.2408 reference
    // (203 nits) in HDR video.
    int mode = 0;
    if (setup_.hdr_video) {
        mode = 1;
        glUniform1f(p.scale, 203.0f / 10000.0f);
    }
    glUniform1i(p.mode, mode);
    glDisable(GL_BLEND);
    for (int pass = 0; pass < 2; pass++) {
        const int pw = target.width[pass], ph = target.height[pass];
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo[pass]);
        // Chroma planes are smaller: the same rectangle, scaled.
        const double sx = double(pw) / std::max(1, target.width[0]);
        const double sy = double(ph) / std::max(1, target.height[0]);
        glViewport(int(x0 * sx), int(y0 * sy), int(fitted.width * sx + 0.5), int(fitted.height * sy + 0.5));
        glUniform1i(p.pass, pass);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

} // namespace atrium::record
