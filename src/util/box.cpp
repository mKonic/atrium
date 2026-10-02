#include "util/box.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

bool box_empty(const Box* box) {
    return !box || box->width <= 0 || box->height <= 0;
}

bool box_intersection(Box* dest, const Box* a, const Box* b) {
    if (box_empty(a) || box_empty(b)) {
        *dest = {};
        return false;
    }
    const int x1 = std::max(a->x, b->x), y1 = std::max(a->y, b->y);
    const int x2 = std::min(a->x + a->width, b->x + b->width);
    const int y2 = std::min(a->y + a->height, b->y + b->height);
    *dest = {x1, y1, x2 - x1, y2 - y1};
    if (box_empty(dest)) {
        *dest = {};
        return false;
    }
    return true;
}

bool box_contains_point(const Box* box, double x, double y) {
    return !box_empty(box) && x >= box->x && x < box->x + box->width && y >= box->y && y < box->y + box->height;
}

bool box_contains_box(const Box* bigger, const Box* smaller) {
    if (box_empty(bigger) || box_empty(smaller))
        return false;
    return smaller->x >= bigger->x && smaller->x + smaller->width <= bigger->x + bigger->width &&
           smaller->y >= bigger->y && smaller->y + smaller->height <= bigger->y + bigger->height;
}

void box_closest_point(const Box* box, double x, double y, double* dest_x, double* dest_y) {
    if (box_empty(box)) {
        *dest_x = *dest_y = NAN;
        return;
    }
    // Width and height are exclusive: (99.9, 99.9) is in a 100x100 box at
    // the origin, (100, 100) isn't. 1/256 keeps clear of that edge without
    // rounding to zero in wl_fixed.
    *dest_x = std::clamp(x, double(box->x), box->x + box->width - 1 / 256.0);
    *dest_y = std::clamp(y, double(box->y), box->y + box->height - 1 / 256.0);
}

bool box_equal(const Box* a, const Box* b) {
    if (box_empty(a))
        a = nullptr;
    if (box_empty(b))
        b = nullptr;
    if (!a || !b)
        return a == b;
    return *a == *b;
}

namespace {

template <class B, class N>
void transform_box(B* dest, const B* box, wl_output_transform transform, N width, N height) {
    const B src = box ? *box : B{};
    if (transform % 2 == 0) {
        dest->width = src.width;
        dest->height = src.height;
    } else {
        dest->width = src.height;
        dest->height = src.width;
    }
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
        dest->x = src.x;
        dest->y = src.y;
        break;
    case WL_OUTPUT_TRANSFORM_90:
        dest->x = height - src.y - src.height;
        dest->y = src.x;
        break;
    case WL_OUTPUT_TRANSFORM_180:
        dest->x = width - src.x - src.width;
        dest->y = height - src.y - src.height;
        break;
    case WL_OUTPUT_TRANSFORM_270:
        dest->x = src.y;
        dest->y = width - src.x - src.width;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        dest->x = width - src.x - src.width;
        dest->y = src.y;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        dest->x = src.y;
        dest->y = src.x;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        dest->x = src.x;
        dest->y = height - src.y - src.height;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        dest->x = height - src.y - src.height;
        dest->y = width - src.x - src.width;
        break;
    }
}

} // namespace

void box_transform(Box* dest, const Box* box, wl_output_transform transform, int width, int height) {
    transform_box(dest, box, transform, width, height);
}

bool fbox_empty(const FBox* box) {
    return !box || box->width <= 0 || box->height <= 0;
}

bool fbox_equal(const FBox* a, const FBox* b) {
    if (fbox_empty(a))
        a = nullptr;
    if (fbox_empty(b))
        b = nullptr;
    if (!a || !b)
        return a == b;
    return *a == *b;
}

void fbox_transform(FBox* dest, const FBox* box, wl_output_transform transform, double width, double height) {
    transform_box(dest, box, transform, width, height);
}

wl_output_transform output_transform_invert(wl_output_transform tr) {
    if ((tr & WL_OUTPUT_TRANSFORM_90) && !(tr & WL_OUTPUT_TRANSFORM_FLIPPED))
        tr = wl_output_transform(tr ^ WL_OUTPUT_TRANSFORM_180);
    return tr;
}

wl_output_transform output_transform_compose(wl_output_transform tr_a, wl_output_transform tr_b) {
    const uint32_t flipped = (tr_a ^ tr_b) & WL_OUTPUT_TRANSFORM_FLIPPED;
    const uint32_t rotation = WL_OUTPUT_TRANSFORM_90 | WL_OUTPUT_TRANSFORM_180;
    // A rotation by k then a flip is a flip then a rotation by -k.
    const uint32_t rotated = (tr_b & WL_OUTPUT_TRANSFORM_FLIPPED) ? (uint32_t(tr_b) - uint32_t(tr_a)) & rotation
                                                                  : (uint32_t(tr_a) + uint32_t(tr_b)) & rotation;
    return wl_output_transform(flipped | rotated);
}

void output_transform_coords(wl_output_transform tr, int* x, int* y) {
    if (tr & WL_OUTPUT_TRANSFORM_90)
        std::swap(*x, *y);
}

} // namespace atrium
