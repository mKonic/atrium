#pragma once
#include "listener.hpp"
#include "scene/scene.hpp"
#include "wl/compositor.hpp"

#include <memory>

#include <unordered_map>
#include <vector>


namespace atrium {

class Server;
struct Config;
namespace wl {
class AtriumGlassManagerV1;
class AtriumGlassV1;
}

// One piece of Liquid Glass on a surface: a rounded rectangle in surface
// coordinates, and how opaque the glass is (it fades with its panel).
struct GlassShape {
    float x, y, width, height, radius, opacity;
    float clip_x = 0, clip_y = 0, clip_width = 0, clip_height = 0;  // where it shows; 0 wide: all
};

// How far past its shapes glass reaches: its shadow, the shape blurred by a
// Gaussian of a bevel / 2.5, fades out by three of those.
constexpr int kGlassShadowReach = 24;

// Make `blur` Liquid Glass in `shapes` (surface coordinates, offset by dx, dy
// in the blur node's), `width` x `height` being the surface's size. `lensing`
// (0..1) grows the bending in as the glass appears. The same material on the
// shell's panels and on atrium's own windows.
void apply_glass(scene::Blur* blur, const std::vector<GlassShape>& shapes, float dx, float dy, int width,
                 int height, double lensing, const Config& c);


// atrium-glass-v1: the shell says exactly where its glass is, so the glass
// is drawn from that geometry (an exact distance to its edge: smooth rims,
// lens and shadow at any size) instead of being guessed from the pixels.
// Double-buffered with the surface's commit, so it moves with its panel.
class GlassShapes {
public:
    explicit GlassShapes(Server& server);
    ~GlassShapes();
    GlassShapes(const GlassShapes&) = delete;
    GlassShapes& operator=(const GlassShapes&) = delete;

    // The shapes `surface` committed, or null when it never said.
    const std::vector<GlassShape>* shapes_for(wl::Surface* surface) const;

private:
    struct Glass {
        wl::Weak<wl::AtriumGlassV1> resource;
        wl::Surface* surface;
        std::vector<GlassShape> pending, current;
        bool committed = false;
        wl::Connection commit, destroy;
    };

    void get_glass(wl::AtriumGlassManagerV1* manager, uint32_t id, wl_resource* surface);
    static void take_shapes(Glass* glass, wl_array* shapes, size_t stride);
    void surface_gone(Glass* glass);
    void glass_gone(Glass* glass);
    static void refresh(wl::Surface* surface);

    Server& server_;
    std::unique_ptr<wl::Global> global_;
    std::vector<wl::Weak<wl::AtriumGlassManagerV1>> managers_;
    std::unordered_map<wl::Surface*, Glass*> glass_;
    std::vector<Glass*> all_;  // including ones whose surface is gone
};

} // namespace atrium
