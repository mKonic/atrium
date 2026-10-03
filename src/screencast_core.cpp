#include "screencast_core.hpp"

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>

#include <algorithm>

namespace atrium {

namespace {

// Byte order: DRM names a 32-bit word from its top, spa names bytes in
// memory, so ARGB8888 (little endian) is BGRA.
constexpr struct {
    uint32_t drm;
    spa_video_format spa;
} kFormats[] = {
    {DRM_FORMAT_ARGB8888, SPA_VIDEO_FORMAT_BGRA}, {DRM_FORMAT_XRGB8888, SPA_VIDEO_FORMAT_BGRx},
    {DRM_FORMAT_ABGR8888, SPA_VIDEO_FORMAT_RGBA}, {DRM_FORMAT_XBGR8888, SPA_VIDEO_FORMAT_RGBx},
    {DRM_FORMAT_RGBA8888, SPA_VIDEO_FORMAT_ABGR}, {DRM_FORMAT_RGBX8888, SPA_VIDEO_FORMAT_xBGR},
    {DRM_FORMAT_BGRA8888, SPA_VIDEO_FORMAT_ARGB}, {DRM_FORMAT_BGRX8888, SPA_VIDEO_FORMAT_xRGB},
};

} // namespace

uint32_t screencast_spa_format(uint32_t drm_format) {
    for (const auto& f : kFormats)
        if (f.drm == drm_format)
            return f.spa;
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

uint32_t screencast_drm_format(uint32_t spa_format) {
    for (const auto& f : kFormats)
        if (uint32_t(f.spa) == spa_format)
            return f.drm;
    return DRM_FORMAT_INVALID;
}

std::vector<uint64_t> screencast_modifiers(const std::vector<uint64_t>& offered,
                                           const std::vector<uint64_t>& supported) {
    std::vector<uint64_t> out;
    for (uint64_t m : offered)
        if (std::ranges::find(supported, m) != supported.end() && std::ranges::find(out, m) == out.end())
            out.push_back(m);
    if (out.size() > 1)
        std::erase(out, DRM_FORMAT_MOD_INVALID);
    return out;
}

int64_t screencast_wait_ns(int64_t last_ns, int64_t now_ns, uint32_t num, uint32_t den) {
    if (num == 0 || den == 0 || last_ns == 0)
        return 0;
    const int64_t interval = int64_t(1'000'000'000) * den / num;
    // A little early is fine: frames come on the screen's refresh, which
    // jitters, and a frame held back a whole refresh halves the rate.
    const int64_t due = last_ns + interval - interval / 8;
    return now_ns >= due ? 0 : due - now_ns;
}

} // namespace atrium
