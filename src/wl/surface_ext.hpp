#pragma once
#include "wl/compositor.hpp"

#include <map>

namespace atrium::wl {

// The small per-surface protocols: each adds a little double-buffered state
// to wl_surface (SurfaceState) and has one object per surface.

// wp_viewporter: crop and scale a surface's buffer.
class Viewporter {
public:
    explicit Viewporter(wl_display* display);
    ~Viewporter();

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Surface*, Resource*> viewports_;
};

// wp_fractional_scale_manager_v1: tell a surface its screen's exact scale.
class FractionalScales {
public:
    explicit FractionalScales(wl_display* display);
    ~FractionalScales();

    void set_preferred_scale(Surface* surface, double scale);

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
    std::map<Surface*, Resource*> scales_;
    std::map<Surface*, double> preferred_;
};

// wp_alpha_modifier_v1, wp_content_type_manager_v1 and
// wp_tearing_control_manager_v1: an opacity, what the content is (a game, a
// video) and whether it may tear.
class SurfaceHints {
public:
    explicit SurfaceHints(wl_display* display);
    ~SurfaceHints();

private:
    std::unique_ptr<Global> alpha_, content_, tearing_;
    std::vector<Weak<Resource>> managers_;
    std::map<Surface*, Resource*> alphas_, contents_, tearings_;
};

// wp_single_pixel_buffer_manager_v1: a 1x1 buffer of one colour, scaled up
// with a viewport (a solid backdrop without allocating one).
class SinglePixelBuffers {
public:
    explicit SinglePixelBuffers(wl_display* display);
    ~SinglePixelBuffers();

    // The colour of a single-pixel buffer (straight RGBA, 0..1), if it is one.
    static bool color_of(Buffer* buffer, float rgba[4]);

private:
    std::unique_ptr<Global> global_;
    std::vector<Weak<Resource>> managers_;
};

} // namespace atrium::wl
