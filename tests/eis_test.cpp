#include "eis_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::eis;

namespace {

// Two screens side by side: 1920x1080 at 0,0 and 1280x1024 at 1920,0.
const std::vector<Zone> kZones{{0, 0, 1920, 1080}, {1920, 0, 1280, 1024}};

} // namespace

TEST(Eis, BarriersOnOuterEdges) {
    EXPECT_TRUE(valid({1, 0, 0, 0, 1079}, kZones));          // the left screen's left edge
    EXPECT_TRUE(valid({2, 3200, 0, 3200, 1023}, kZones));    // the right screen's right edge
    EXPECT_TRUE(valid({3, 0, 0, 1919, 0}, kZones));          // the top of the left screen
    EXPECT_TRUE(valid({4, 1920, 1080, 0, 1080}, kZones));    // its bottom, either way round
    EXPECT_TRUE(valid({5, 1920, 1024, 1920, 1080}, kZones)); // the left screen's right edge below the other
}

TEST(Eis, BarriersElsewhereAreRefused) {
    EXPECT_FALSE(valid({1, 1920, 0, 1920, 1000}, kZones));  // between the two screens
    EXPECT_FALSE(valid({2, 0, 0, 100, 100}, kZones));       // diagonal
    EXPECT_FALSE(valid({3, 500, 0, 500, 1079}, kZones));    // through the middle
    EXPECT_FALSE(valid({4, 0, 0, 0, 2000}, kZones));        // longer than the edge
    EXPECT_FALSE(valid({5, 10, 10, 10, 10}, kZones));       // a point
    EXPECT_FALSE(valid({6, 0, 0, 0, 1079}, {}));            // no screens
}

TEST(Eis, PushingThroughABarrier) {
    const std::vector<Barrier> barriers{{7, 0, 0, 0, 1079}, {8, 3200, 0, 3200, 1023}, {9, 0, 0, 1919, 0}};
    // At the left edge, moving left: through barrier 7.
    EXPECT_EQ(crossed(barriers, 0, 500, -3, 0), 7u);
    // Moving along it, or away, is not.
    EXPECT_EQ(crossed(barriers, 0, 500, 0, 5), std::nullopt);
    EXPECT_EQ(crossed(barriers, 0, 500, 4, 0), std::nullopt);
    // Past where it ends.
    EXPECT_EQ(crossed(barriers, 0, 1200, -3, 0), std::nullopt);
    // The pointer rests a hair inside the right edge; moving right goes through.
    EXPECT_EQ(crossed(barriers, 3199.99, 300, 2, 0), 8u);
    EXPECT_EQ(crossed(barriers, 3100, 300, 2, 0), std::nullopt);
    // Up through the top.
    EXPECT_EQ(crossed(barriers, 800, 0, 0, -1), 9u);
}
