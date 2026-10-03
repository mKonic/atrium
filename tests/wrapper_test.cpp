#include "../src/wrapper.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

TEST(Wrapper, GivesUpOnThreeCrashesInAMinute) {
    const double spread[] = {0, 100, 200};
    EXPECT_FALSE(too_many_crashes(spread, 3, 210));
    const double close[] = {100, 130, 150};
    EXPECT_TRUE(too_many_crashes(close, 3, 151));
    const double two[] = {100, 101};
    EXPECT_FALSE(too_many_crashes(two, 2, 102));
    // Old crashes age out.
    const double mixed[] = {0, 10, 100, 101};
    EXPECT_FALSE(too_many_crashes(mixed, 4, 102));
}

} // namespace
