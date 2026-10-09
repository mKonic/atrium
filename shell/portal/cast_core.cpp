#include "cast_core.hpp"

#include <drm_fourcc.h>

#include <algorithm>

namespace atrium::cast {

namespace {

struct FormatPair {
    uint32_t drm;
    spa_video_format pw;
};
// DRM names bytes from the most significant end, PipeWire from memory order.
constexpr FormatPair kFormats[] = {
    {DRM_FORMAT_ARGB8888, SPA_VIDEO_FORMAT_BGRA},       {DRM_FORMAT_XRGB8888, SPA_VIDEO_FORMAT_BGRx},
    {DRM_FORMAT_RGBA8888, SPA_VIDEO_FORMAT_ABGR},       {DRM_FORMAT_RGBX8888, SPA_VIDEO_FORMAT_xBGR},
    {DRM_FORMAT_ABGR8888, SPA_VIDEO_FORMAT_RGBA},       {DRM_FORMAT_XBGR8888, SPA_VIDEO_FORMAT_RGBx},
    {DRM_FORMAT_BGRA8888, SPA_VIDEO_FORMAT_ARGB},       {DRM_FORMAT_BGRX8888, SPA_VIDEO_FORMAT_xRGB},
    {DRM_FORMAT_NV12, SPA_VIDEO_FORMAT_NV12},           {DRM_FORMAT_XRGB2101010, SPA_VIDEO_FORMAT_xRGB_210LE},
    {DRM_FORMAT_XBGR2101010, SPA_VIDEO_FORMAT_xBGR_210LE}, {DRM_FORMAT_RGBX1010102, SPA_VIDEO_FORMAT_RGBx_102LE},
    {DRM_FORMAT_BGRX1010102, SPA_VIDEO_FORMAT_BGRx_102LE}, {DRM_FORMAT_ARGB2101010, SPA_VIDEO_FORMAT_ARGB_210LE},
    {DRM_FORMAT_ABGR2101010, SPA_VIDEO_FORMAT_ABGR_210LE}, {DRM_FORMAT_RGBA1010102, SPA_VIDEO_FORMAT_RGBA_102LE},
    {DRM_FORMAT_BGRA1010102, SPA_VIDEO_FORMAT_BGRA_102LE}, {DRM_FORMAT_BGR888, SPA_VIDEO_FORMAT_RGB},
    {DRM_FORMAT_RGB888, SPA_VIDEO_FORMAT_BGR},
};

} // namespace

spa_video_format pw_from_drm(uint32_t fourcc) {
    for (const auto& f : kFormats)
        if (f.drm == fourcc)
            return f.pw;
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

uint32_t drm_from_pw(spa_video_format format) {
    for (const auto& f : kFormats)
        if (f.pw == format)
            return f.drm;
    return DRM_FORMAT_INVALID;
}

spa_video_format strip_alpha(spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return SPA_VIDEO_FORMAT_BGRx;
    case SPA_VIDEO_FORMAT_ABGR: return SPA_VIDEO_FORMAT_xBGR;
    case SPA_VIDEO_FORMAT_RGBA: return SPA_VIDEO_FORMAT_RGBx;
    case SPA_VIDEO_FORMAT_ARGB: return SPA_VIDEO_FORMAT_xRGB;
    case SPA_VIDEO_FORMAT_ARGB_210LE: return SPA_VIDEO_FORMAT_xRGB_210LE;
    case SPA_VIDEO_FORMAT_ABGR_210LE: return SPA_VIDEO_FORMAT_xBGR_210LE;
    case SPA_VIDEO_FORMAT_RGBA_102LE: return SPA_VIDEO_FORMAT_RGBx_102LE;
    case SPA_VIDEO_FORMAT_BGRA_102LE: return SPA_VIDEO_FORMAT_BGRx_102LE;
    default: return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

int bytes_per_pixel(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_BGR888:
    case DRM_FORMAT_RGB888: return 3;
    case DRM_FORMAT_NV12: return -1;
    default: return pw_from_drm(fourcc) == SPA_VIDEO_FORMAT_UNKNOWN ? -1 : 4;
    }
}

uint32_t drm_from_shm(uint32_t shm) {
    switch (shm) {
    case 0: return DRM_FORMAT_ARGB8888;
    case 1: return DRM_FORMAT_XRGB8888;
    default: return shm;
    }
}

uint32_t shm_from_drm(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888: return 0;
    case DRM_FORMAT_XRGB8888: return 1;
    default: return fourcc;
    }
}

Rect merge(const Rect& a, const Rect& b) {
    const int32_t x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int32_t x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

std::vector<Rect> fit_damage(const std::vector<Rect>& damage, size_t count) {
    if (count == 0 || damage.size() <= count)
        return count == 0 ? std::vector<Rect>{} : damage;
    std::vector<Rect> out(damage.begin(), damage.begin() + long(count - 1));
    Rect rest = damage[count - 1];
    for (size_t i = count; i < damage.size(); i++)
        rest = merge(rest, damage[i]);
    out.push_back(rest);
    return out;
}

uint64_t frame_delay_ns(double max_fps, int64_t elapsed_ns) {
    if (max_fps <= 0)
        return 0;
    const int64_t target = int64_t(1e9 / max_fps);
    return elapsed_ns < target ? uint64_t(target - elapsed_ns) : 0;
}

std::optional<std::string> match_window(const Choice& c, const std::vector<Candidate>& windows) {
    if (c.app_id.empty())
        return std::nullopt;
    const Candidate* only = nullptr;
    int same_app = 0;
    for (const Candidate& w : windows) {
        if (w.app_id != c.app_id)
            continue;
        if (w.title == c.title)
            return w.identifier;
        only = &w;
        same_app++;
    }
    if (same_app == 1)
        return only->identifier;
    return std::nullopt;
}

} // namespace atrium::cast
