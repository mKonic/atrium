#include "warp.hpp"

#include <gtest/gtest.h>

using namespace atrium::warp;

namespace {

// A 400x300 window at 100,100 pouring into a 48 px icon at 500,900.
std::pair<double, double> g(double t, double u, double v) {
    return genie(100, 100, 400, 300, 500, 900, 48, t, u, v);
}

} // namespace

TEST(Genie, AtRestTheWindowIsWhereItIs) {
    auto [x0, y0] = g(0, 0, 0);
    auto [x1, y1] = g(0, 1, 1);
    EXPECT_DOUBLE_EQ(x0, 100);
    EXPECT_DOUBLE_EQ(y0, 100);
    EXPECT_DOUBLE_EQ(x1, 500);
    EXPECT_DOUBLE_EQ(y1, 400);
}

TEST(Genie, AtTheEndItIsAllInTheIcon) {
    for (double u : {0.0, 0.5, 1.0})
        for (double v : {0.0, 0.5, 1.0}) {
            auto [x, y] = g(1, u, v);
            EXPECT_DOUBLE_EQ(y, 900);
            EXPECT_GE(x, 500 - 24 - 1e-9);
            EXPECT_LE(x, 500 + 24 + 1e-9);
        }
}

TEST(Genie, HalfwayTheBottomIsNarrowerThanTheTop) {
    const double top = g(0.5, 1, 0).first - g(0.5, 0, 0).first;
    const double bottom = g(0.5, 1, 1).first - g(0.5, 0, 1).first;
    EXPECT_LT(bottom, top);
    EXPECT_GT(top, 48);
}

TEST(Genie, HalfwayTheBottomHasReachedTheIcon) {
    auto [x, y] = g(0.5, 0.5, 1);
    EXPECT_DOUBLE_EQ(y, 900);
    EXPECT_NEAR(x, 500, 1e-9);
    EXPECT_DOUBLE_EQ(g(0.5, 0.5, 0).second, 100);  // the top hasn't moved yet
}

TEST(Genie, ItNeverGoesPastTheIcon) {
    for (double t = 0; t <= 1; t += 0.1)
        EXPECT_LE(g(t, 0.5, 1).second, 900 + 1e-9);
}

TEST(Mesh, ACellIsTwoTrianglesOverThePiece) {
    const Fn flat = [](double u, double v) { return std::pair{u * 100, v * 50}; };
    auto m = mesh(flat, 0, 0, 1, 1, 1);
    ASSERT_EQ(m.size(), 6u);
    EXPECT_FLOAT_EQ(m[0].x, 0);
    EXPECT_FLOAT_EQ(m[5].x, 100);
    EXPECT_FLOAT_EQ(m[5].y, 50);
    EXPECT_FLOAT_EQ(m[5].u, 1);
}

TEST(Mesh, APieceMapsItsOwnPartOfTheFrame) {
    // The lower half of the frame: v runs 0.5..1 in the frame, 0..1 in the piece.
    const Fn flat = [](double u, double v) { return std::pair{u * 100, v * 100}; };
    auto m = mesh(flat, 0, 0.5, 1, 1, 4);
    ASSERT_EQ(m.size(), 4u * 4u * 6u);
    EXPECT_FLOAT_EQ(m.front().y, 50);
    EXPECT_FLOAT_EQ(m.front().v, 0);
    EXPECT_FLOAT_EQ(m.back().y, 100);
    EXPECT_FLOAT_EQ(m.back().v, 1);
}
