#pragma once
// Rectangles, edges and output transforms. The helpers keep wlroots' C-style
// shapes (util/box.c, util/transform.c; MIT), which atrium's code was written
// against.
#include <wayland-server-protocol.h>

#include <cstdint>

namespace atrium {

struct Box {
    int x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    bool operator==(const Box&) const = default;
};

struct FBox {
    double x = 0, y = 0, width = 0, height = 0;
    bool empty() const { return width <= 0 || height <= 0; }
    static FBox of(const Box& b) { return {double(b.x), double(b.y), double(b.width), double(b.height)}; }
    bool operator==(const FBox&) const = default;
};

// A side or corner, as bits (xdg_toplevel's resize edges are made of these).
enum Edges : uint32_t {
    EDGE_NONE = 0,
    EDGE_TOP = 1,
    EDGE_BOTTOM = 2,
    EDGE_LEFT = 4,
    EDGE_RIGHT = 8,
};

bool box_empty(const Box* box);
// False (and `dest` zeroed) when they don't overlap.
bool box_intersection(Box* dest, const Box* a, const Box* b);
bool box_contains_point(const Box* box, double x, double y);
bool box_contains_box(const Box* bigger, const Box* smaller);
// The point of `box` nearest (x, y), inside its right and bottom edges by 1/256.
void box_closest_point(const Box* box, double x, double y, double* dest_x, double* dest_y);
// Empty boxes are all equal.
bool box_equal(const Box* a, const Box* b);
// `box` within a (width x height) area turned by `transform`.
void box_transform(Box* dest, const Box* box, wl_output_transform transform, int width, int height);

bool fbox_empty(const FBox* box);
bool fbox_equal(const FBox* a, const FBox* b);
void fbox_transform(FBox* dest, const FBox* box, wl_output_transform transform, double width, double height);

wl_output_transform output_transform_invert(wl_output_transform tr);
// `tr_a`, then `tr_b`.
wl_output_transform output_transform_compose(wl_output_transform tr_a, wl_output_transform tr_b);
// Swaps x and y under a quarter turn.
void output_transform_coords(wl_output_transform tr, int* x, int* y);

} // namespace atrium
