#pragma once
// pixman region helpers, after wlroots' util/region.c (MIT).
#include <pixman.h>
#include <wayland-server-protocol.h>

namespace atrium {

void region_scale(pixman_region32_t* dst, const pixman_region32_t* src, float scale);
void region_scale_xy(pixman_region32_t* dst, const pixman_region32_t* src, float scale_x, float scale_y);
// `src` within a (width x height) area turned by `transform`.
void region_transform(pixman_region32_t* dst, const pixman_region32_t* src, wl_output_transform transform, int width,
                      int height);
// Each rectangle grown by `distance` on every side.
void region_expand(pixman_region32_t* dst, const pixman_region32_t* src, int distance);
// Moving from (x1, y1) toward (x2, y2) without leaving `region`: where it
// stops. False if (x1, y1) isn't in it.
bool region_confine(const pixman_region32_t* region, double x1, double y1, double x2, double y2, double* x2_out,
                    double* y2_out);

} // namespace atrium
