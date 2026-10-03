#include "screenshot_core.hpp"

#include <algorithm>
#include <cstring>

namespace atrium {

Box screenshot_area(const std::vector<Box>& outputs, const std::optional<Box>& region) {
    Box all{};
    for (const Box& o : outputs) {
        if (o.width <= 0 || o.height <= 0)
            continue;
        if (all.width <= 0) {
            all = o;
            continue;
        }
        const int x2 = std::max(all.x + all.width, o.x + o.width), y2 = std::max(all.y + all.height, o.y + o.height);
        all.x = std::min(all.x, o.x);
        all.y = std::min(all.y, o.y);
        all.width = x2 - all.x;
        all.height = y2 - all.y;
    }
    if (!region)
        return all;
    Box out{};
    if (!box_intersection(&out, &*region, &all))
        return {};
    return out;
}

std::vector<uint32_t> screenshot_upright(const uint8_t* data, int bw, int bh, size_t stride,
                                         wl_output_transform transform, int* width, int* height) {
    const bool sideways = transform & WL_OUTPUT_TRANSFORM_90;
    const int w = sideways ? bh : bw, h = sideways ? bw : bh;
    *width = w;
    *height = h;
    std::vector<uint32_t> out(size_t(w) * size_t(h));
    if (transform == WL_OUTPUT_TRANSFORM_NORMAL) {
        for (int y = 0; y < h; ++y)
            std::memcpy(&out[size_t(y) * size_t(w)], data + size_t(y) * stride, size_t(w) * 4);
        return out;
    }
    // Where each shown pixel sits in the buffer (as capture's buffer_box
    // finds a region): the same turn, one pixel at a time.
    const wl_output_transform back = output_transform_invert(transform);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Box b{x, y, 1, 1};
            box_transform(&b, &b, back, w, h);
            uint32_t px;
            std::memcpy(&px, data + size_t(b.y) * stride + size_t(b.x) * 4, 4);
            out[size_t(y) * size_t(w) + size_t(x)] = px;
        }
    return out;
}

} // namespace atrium
