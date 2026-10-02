// Which GPU drives the screens first.
#include "backend/session.hpp"

#include <gtest/gtest.h>

using atrium::backend::GpuCandidate;
using atrium::backend::order_gpus;

// A hybrid laptop: the firmware booted on the integrated GPU, the one
// wired to the built-in panel, though udev lists the discrete one first.
TEST(BackendSession, TheBootGpuComesFirst) {
    EXPECT_EQ(order_gpus({{"/dev/dri/card0", false}, {"/dev/dri/card1", true}, {"/dev/dri/card2", false}}, nullptr),
              (std::vector<std::string>{"/dev/dri/card1", "/dev/dri/card0", "/dev/dri/card2"}));
}

// The environment's list wins outright, in its own order.
TEST(BackendSession, TheEnvironmentListOverrides) {
    EXPECT_EQ(order_gpus({{"/dev/dri/card0", true}}, "/dev/dri/card2::/dev/dri/card0"),
              (std::vector<std::string>{"/dev/dri/card2", "/dev/dri/card0"}));
    EXPECT_EQ(order_gpus({{"/dev/dri/card0", false}}, ""), (std::vector<std::string>{"/dev/dri/card0"}));
}
