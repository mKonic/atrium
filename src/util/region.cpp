#include "util/region.hpp"

#include <cassert>
#include <cmath>
#include <vector>

namespace atrium {

namespace {

// `dst` made of `src`'s rectangles, each mapped by `f`.
template <class F>
void map_rects(pixman_region32_t* dst, const pixman_region32_t* src, F f) {
    int n = 0;
    const pixman_box32_t* in = pixman_region32_rectangles(const_cast<pixman_region32_t*>(src), &n);
    std::vector<pixman_box32_t> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        out[size_t(i)] = f(in[i]);
    pixman_region32_fini(dst);
    pixman_region32_init_rects(dst, out.data(), n);
}

} // namespace

void region_scale(pixman_region32_t* dst, const pixman_region32_t* src, float scale) {
    region_scale_xy(dst, src, scale, scale);
}

void region_scale_xy(pixman_region32_t* dst, const pixman_region32_t* src, float sx, float sy) {
    if (sx == 1.0f && sy == 1.0f) {
        pixman_region32_copy(dst, const_cast<pixman_region32_t*>(src));
        return;
    }
    map_rects(dst, src, [&](const pixman_box32_t& r) {
        return pixman_box32_t{int32_t(std::floor(r.x1 * sx)), int32_t(std::floor(r.y1 * sy)),
                              int32_t(std::ceil(r.x2 * sx)), int32_t(std::ceil(r.y2 * sy))};
    });
}

void region_transform(pixman_region32_t* dst, const pixman_region32_t* src, wl_output_transform transform, int width,
                      int height) {
    if (transform == WL_OUTPUT_TRANSFORM_NORMAL) {
        pixman_region32_copy(dst, const_cast<pixman_region32_t*>(src));
        return;
    }
    map_rects(dst, src, [&](const pixman_box32_t& r) -> pixman_box32_t {
        switch (transform) {
        case WL_OUTPUT_TRANSFORM_90:
            return {height - r.y2, r.x1, height - r.y1, r.x2};
        case WL_OUTPUT_TRANSFORM_180:
            return {width - r.x2, height - r.y2, width - r.x1, height - r.y1};
        case WL_OUTPUT_TRANSFORM_270:
            return {r.y1, width - r.x2, r.y2, width - r.x1};
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            return {width - r.x2, r.y1, width - r.x1, r.y2};
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            return {r.y1, r.x1, r.y2, r.x2};
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            return {r.x1, height - r.y2, r.x2, height - r.y1};
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            return {height - r.y2, width - r.x2, height - r.y1, width - r.x1};
        default:
            return r;
        }
    });
}

void region_expand(pixman_region32_t* dst, const pixman_region32_t* src, int distance) {
    assert(distance >= 0);
    if (distance == 0) {
        pixman_region32_copy(dst, const_cast<pixman_region32_t*>(src));
        return;
    }
    map_rects(dst, src, [&](const pixman_box32_t& r) {
        return pixman_box32_t{r.x1 - distance, r.y1 - distance, r.x2 + distance, r.y2 + distance};
    });
}

namespace {

void confine(const pixman_region32_t* region, double x1, double y1, double x2, double y2, double* x2_out,
             double* y2_out, pixman_box32_t box) {
    auto* rg = const_cast<pixman_region32_t*>(region);
    const double x_clamped = std::fmax(std::fmin(x2, box.x2 - 1), box.x1);
    const double y_clamped = std::fmax(std::fmin(y2, box.y2 - 1), box.y1);
    // Past box.{x,y}2 - 1 but short of box.{x,y}2 is still inside.
    if (std::floor(x_clamped) == std::floor(x2) && std::floor(y_clamped) == std::floor(y2)) {
        *x2_out = x2;
        *y2_out = y2;
        return;
    }
    const double dx = x2 - x1, dy = y2 - y1;
    // fabs keeps negative zeroes (and so negative infinity) out.
    const double delta = std::fmin(std::fabs(x_clamped - x1) / std::fabs(dx), std::fabs(y_clamped - y1) / std::fabs(dy));
    // Clamped again for rounding.
    const double x = std::fmax(std::fmin(delta * dx + x1, box.x2 - 1), box.x1);
    const double y = std::fmax(std::fmin(delta * dy + y1, box.y2 - 1), box.y1);
    // One unit past the boundary: a neighbouring box?
    const int x_ext = int(std::floor(x)) + (dx == 0 ? 0 : dx > 0 ? 1 : -1);
    const int y_ext = int(std::floor(y)) + (dy == 0 ? 0 : dy > 0 ? 1 : -1);
    if (pixman_region32_contains_point(rg, x_ext, y_ext, &box)) {
        confine(region, x, y, x2, y2, x2_out, y2_out, box);
    } else if (dx == 0 || dy == 0) {
        *x2_out = x;
        *y2_out = y;
    } else {
        const bool bordering_x = x == box.x1 || x == box.x2 - 1;
        const bool bordering_y = y == box.y1 || y == box.y2 - 1;
        if (bordering_x == bordering_y) {
            double x2p, y2p, t1, t2;
            confine(region, x, y, x, y2, &t1, &y2p, box);
            confine(region, x, y, x2, y, &x2p, &t2, box);
            if (std::fabs(x2p - x) > std::fabs(y2p - y)) {
                *x2_out = x2p;
                *y2_out = y;
            } else {
                *x2_out = x;
                *y2_out = y2p;
            }
        } else if (bordering_x) {
            confine(region, x, y, x, y2, x2_out, y2_out, box);
        } else {
            confine(region, x, y, x2, y, x2_out, y2_out, box);
        }
    }
}

} // namespace

bool region_confine(const pixman_region32_t* region, double x1, double y1, double x2, double y2, double* x2_out,
                    double* y2_out) {
    pixman_box32_t box;
    if (!pixman_region32_contains_point(const_cast<pixman_region32_t*>(region), int(std::floor(x1)),
                                        int(std::floor(y1)), &box))
        return false;
    confine(region, x1, y1, x2, y2, x2_out, y2_out, box);
    return true;
}

} // namespace atrium
