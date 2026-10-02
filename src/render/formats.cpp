#include "render/formats.hpp"

#include <drm_fourcc.h>

namespace atrium::render {

namespace {

constexpr PixelFormat kFormats[] = {
    {DRM_FORMAT_ARGB8888, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 4, true},
    {DRM_FORMAT_XRGB8888, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 4, false},
    {DRM_FORMAT_XBGR8888, 0, GL_RGBA, GL_UNSIGNED_BYTE, 4, false},
    {DRM_FORMAT_ABGR8888, 0, GL_RGBA, GL_UNSIGNED_BYTE, 4, true},
    {DRM_FORMAT_BGR888, 0, GL_RGB, GL_UNSIGNED_BYTE, 3, false},
    {DRM_FORMAT_RGBX4444, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2, false},
    {DRM_FORMAT_RGBA4444, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2, true},
    {DRM_FORMAT_RGBX5551, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, false},
    {DRM_FORMAT_RGBA5551, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, true},
    {DRM_FORMAT_RGB565, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, false},
    {DRM_FORMAT_XBGR2101010, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV_EXT, 4, false},
    {DRM_FORMAT_ABGR2101010, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV_EXT, 4, true},
    {DRM_FORMAT_BGR161616F, 0, GL_RGB, GL_HALF_FLOAT_OES, 6, false},
    {DRM_FORMAT_XBGR16161616F, 0, GL_RGBA, GL_HALF_FLOAT_OES, 8, false},
    {DRM_FORMAT_ABGR16161616F, 0, GL_RGBA, GL_HALF_FLOAT_OES, 8, true},
    {DRM_FORMAT_BGR161616, GL_RGB16_EXT, GL_RGB, GL_UNSIGNED_SHORT, 6, false},
    {DRM_FORMAT_XBGR16161616, GL_RGBA16_EXT, GL_RGBA, GL_UNSIGNED_SHORT, 8, false},
    {DRM_FORMAT_ABGR16161616, GL_RGBA16_EXT, GL_RGBA, GL_UNSIGNED_SHORT, 8, true},
};

} // namespace

const PixelFormat* format_from_drm(uint32_t drm) {
    for (const PixelFormat& f : kFormats)
        if (f.drm == drm)
            return &f;
    return nullptr;
}

const PixelFormat* format_from_gl(GLint gl_format, GLint gl_type, bool alpha) {
    for (const PixelFormat& f : kFormats)
        if (f.gl_format == gl_format && f.gl_type == gl_type && f.has_alpha == alpha)
            return &f;
    return nullptr;
}

bool drm_format_has_alpha(uint32_t drm) {
    if (const PixelFormat* f = format_from_drm(drm))
        return f->has_alpha;
    switch (drm) {
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_BGRA1010102:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_ABGR16161616:
    case DRM_FORMAT_ARGB1555:
    case DRM_FORMAT_ABGR1555:
    case DRM_FORMAT_ARGB4444:
    case DRM_FORMAT_ABGR4444:
        return true;
    default:
        return false;  // XRGB, YUV and the rest: opaque
    }
}

bool format_supported(const GlCaps& caps, const PixelFormat& f) {
    if (f.gl_type == GL_UNSIGNED_INT_2_10_10_10_REV_EXT)
        return true;  // core in GLES 3
    if (f.gl_type == GL_HALF_FLOAT_OES)
        return caps.OES_texture_half_float_linear;
    if (f.gl_type == GL_UNSIGNED_SHORT)
        return caps.EXT_texture_norm16;
    return true;
}

void shm_formats(const GlCaps& caps, FormatSet* out) {
    for (const PixelFormat& f : kFormats) {
        if (!format_supported(caps, f))
            continue;
        out->add(f.drm, DRM_FORMAT_MOD_INVALID);
        out->add(f.drm, DRM_FORMAT_MOD_LINEAR);
    }
}

} // namespace atrium::render
