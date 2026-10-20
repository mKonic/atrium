#pragma once
// The recorder's GPU side, as gpu-screen-recorder's egl.c and
// color_conversion.c: an EGL context on the GPU the frames live on, the
// captured dmabufs as textures, and its shader turning RGB into the encoder's
// Y and UV planes (BT.709 with gsr's weights, or BT.2020 PQ), undoing night
// light on the way.

#include "capture.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <map>
#include <string>

namespace atrium::record {

// Where the shader draws: the encoder's luma and chroma planes (chroma at
// half size), as framebuffers.
struct Planes {
    GLuint fbo[2] = {};
    int width[2] = {}, height[2] = {};
};

// How the captured colours become the video's.
struct ColorSetup {
    bool ten_bit = false;     // P010 instead of NV12
    bool hdr_video = false;   // BT.2020 PQ instead of BT.709 (with ten_bit)
    bool full_range = false;
};

// What the screen shows right now (from atrium; gsr reads the same from KDE
// and the CRTC).
struct ScreenColor {
    bool hdr = false;            // in HDR (its copies come as SDR, 10-bit keeps their depth)
    float night[3] = {1, 1, 1};  // night light's linear-light gain still in the copies (1: none)
};

class Gpu {
public:
    ~Gpu();
    // A context on `render_node` (the compositor's GPU).
    bool open(const std::string& render_node, bool debug, std::string& error);
    bool nvidia() const { return nvidia_; }
    const std::string& node() const { return node_; }
    const std::string& vendor() const { return vendor_; }
    EGLDisplay display() const { return display_; }
    bool has_extension(const char* name) const;

    // Shaders for this colour setup.
    bool prepare(const ColorSetup& setup, std::string& error);

    // Draws `frame` (buffer transform `transform`, cropped to `crop` in the
    // upright image's pixels; empty: all of it) into `target`, filling
    // `video` (the picture's part of the planes, from their top left) and
    // keeping its shape.
    bool draw(const Dmabuf& frame, int32_t transform, const int crop[4], const ScreenColor& color, const Planes& target,
              int video_width, int video_height);
    // Clears the planes to black.
    void clear(const Planes& target);

    // A texture of `format` (GL_R8, GL_RG8, GL_R16_EXT, GL_RG16_EXT) and a
    // framebuffer around it.
    bool make_plane(GLenum format, int width, int height, GLuint& texture, GLuint& fbo);
    // A texture from a dmabuf's single plane, in `format` (DRM fourcc), with
    // a framebuffer (VA-API's surfaces).
    bool import_plane(int fd, uint32_t offset, uint32_t stride, uint64_t modifier, uint32_t fourcc, int width, int height,
                      GLuint& texture, GLuint& fbo, EGLImage& image);

    PFNEGLCREATEIMAGEKHRPROC egl_create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = nullptr;

private:
    struct Imported {
        EGLImage image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        bool external = false;
        uint64_t used = 0;
    };
    struct Program {
        GLuint id = 0;
        GLint to_yuv = -1, gain = -1, pass = -1, mode = -1, night = -1, scale = -1;
        GLint map = -1, offset = -1, tex = -1;
    };

    Imported* import(const Dmabuf& frame);
    EGLImage create_image(const Dmabuf& frame, bool modifiers);
    bool build(Program& p, bool external, std::string& error);

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    std::string vendor_, node_;
    bool nvidia_ = false;
    std::string egl_extensions_;
    std::map<uint64_t, Imported> imported_;
    uint64_t use_count_ = 0;
    Program programs_[2];  // sampler2D, samplerExternalOES
    ColorSetup setup_;
    GLuint vao_ = 0;
};

} // namespace atrium::record
