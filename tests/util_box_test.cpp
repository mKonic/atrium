#include "util/box.hpp"
#include "util/region.hpp"

#include <gtest/gtest.h>

using namespace atrium;

TEST(UtilBox, IntersectionAndContainment) {
    const Box a{0, 0, 100, 100}, b{50, 50, 100, 100}, far{200, 200, 10, 10};
    Box d;
    EXPECT_TRUE(box_intersection(&d, &a, &b));
    EXPECT_EQ(d, (Box{50, 50, 50, 50}));
    EXPECT_FALSE(box_intersection(&d, &a, &far));
    EXPECT_TRUE(d.empty());
    EXPECT_TRUE(box_contains_point(&a, 99.9, 0));
    EXPECT_FALSE(box_contains_point(&a, 100, 0));  // width is exclusive
    const Box inner{10, 10, 5, 5};
    EXPECT_TRUE(box_contains_box(&a, &inner));
    const Box e1{}, e2{5, 5, 0, 3};
    EXPECT_TRUE(box_equal(&e1, &e2));  // empties are equal
}

TEST(UtilBox, ClosestPointStaysInside) {
    const Box a{0, 0, 100, 100};
    double x, y;
    box_closest_point(&a, 150, -20, &x, &y);
    EXPECT_DOUBLE_EQ(x, 100 - 1 / 256.0);
    EXPECT_DOUBLE_EQ(y, 0);
}

TEST(UtilBox, TransformsTurnAndInvert) {
    // A 10x20 box at (1, 2) in a 100x50 area, turned a quarter.
    const Box b{1, 2, 10, 20};
    Box d;
    box_transform(&d, &b, WL_OUTPUT_TRANSFORM_90, 100, 50);
    EXPECT_EQ(d, (Box{50 - 2 - 20, 1, 20, 10}));
    box_transform(&d, &b, WL_OUTPUT_TRANSFORM_180, 100, 50);
    EXPECT_EQ(d, (Box{100 - 1 - 10, 50 - 2 - 20, 10, 20}));
    EXPECT_EQ(output_transform_invert(WL_OUTPUT_TRANSFORM_90), WL_OUTPUT_TRANSFORM_270);
    EXPECT_EQ(output_transform_invert(WL_OUTPUT_TRANSFORM_FLIPPED_90), WL_OUTPUT_TRANSFORM_FLIPPED_90);
    for (int t = 0; t < 8; ++t) {
        const auto tr = wl_output_transform(t);
        EXPECT_EQ(output_transform_compose(tr, output_transform_invert(tr)), WL_OUTPUT_TRANSFORM_NORMAL) << t;
    }
    int w = 1920, h = 1080;
    output_transform_coords(WL_OUTPUT_TRANSFORM_270, &w, &h);
    EXPECT_EQ(w, 1080);
}

TEST(UtilRegion, TransformExpandAndConfine) {
    pixman_region32_t r, out;
    pixman_region32_init_rect(&r, 0, 0, 10, 20);
    pixman_region32_init(&out);
    region_transform(&out, &r, WL_OUTPUT_TRANSFORM_90, 100, 50);
    const pixman_box32_t* e = pixman_region32_extents(&out);
    EXPECT_EQ((std::array{e->x1, e->y1, e->x2, e->y2}), (std::array{30, 0, 50, 10}));
    region_expand(&out, &r, 2);
    e = pixman_region32_extents(&out);
    EXPECT_EQ((std::array{e->x1, e->y1, e->x2, e->y2}), (std::array{-2, -2, 12, 22}));
    // Moving right out of the box stops at its last column.
    double x, y;
    EXPECT_TRUE(region_confine(&r, 5, 5, 50, 5, &x, &y));
    EXPECT_DOUBLE_EQ(x, 9);
    EXPECT_DOUBLE_EQ(y, 5);
    EXPECT_FALSE(region_confine(&r, 50, 50, 0, 0, &x, &y));
    pixman_region32_fini(&r);
    pixman_region32_fini(&out);
}
