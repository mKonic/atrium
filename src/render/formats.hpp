#pragma once
#include "render/gl.hpp"

namespace atrium::render {

// How a DRM format uploads to (and reads back from) GL. DRM formats are
// little endian and GL's are byte order, so DRM ARGB8888 is GL BGRA.
struct PixelFormat {
    uint32_t drm;
    GLint gl_internal;  // 0: same as gl_format
    GLint gl_format;
    GLint gl_type;
    int bytes_per_pixel;
    bool has_alpha;
};

const PixelFormat* format_from_drm(uint32_t drm);
const PixelFormat* format_from_gl(GLint gl_format, GLint gl_type, bool alpha);
bool drm_format_has_alpha(uint32_t drm);

struct GlCaps;
// Whether GL here can sample it (the extension its type needs).
bool format_supported(const GlCaps& caps, const PixelFormat& f);
// Every shm format this GL takes, with the implicit and linear modifiers.
void shm_formats(const GlCaps& caps, wlr_drm_format_set* out);

// What this GL context offers beyond GLES 3.0 core.
struct GlCaps {
    bool EXT_read_format_bgra = false;
    bool KHR_debug = false;
    bool OES_egl_image_external = false;
    bool OES_egl_image_external_essl3 = false;
    bool OES_egl_image = false;
    bool EXT_texture_type_2_10_10_10_REV = false;
    bool OES_texture_half_float_linear = false;
    bool EXT_texture_norm16 = false;
    bool EXT_disjoint_timer_query = false;
    bool EXT_color_buffer_half_float = false;
    bool KHR_robustness = false;
};

} // namespace atrium::render
