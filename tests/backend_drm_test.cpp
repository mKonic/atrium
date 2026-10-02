// The DRM backend's pure parts: matching connectors to CRTCs.
#include "backend/drm/match.hpp"

#include <gtest/gtest.h>

using atrium::backend::drm::kUnmatched;
using atrium::backend::drm::match_crtcs;
using V = std::vector<uint32_t>;

// Two screens, two CRTCs, the first screen only on CRTC 1: both get one.
TEST(BackendDrm, EveryScreenGetsACrtcWhereOneFits) {
    EXPECT_EQ(match_crtcs({0b10, 0b11}, {kUnmatched, kUnmatched}), (V{1, 0}));
}

// A screen plugged in doesn't move the one already lit (no modeset for it).
TEST(BackendDrm, ALitScreenKeepsItsCrtc) {
    // Connector 0 was on CRTC 0; connector 1 (new) could use either.
    EXPECT_EQ(match_crtcs({0b11, 0b11}, {0, kUnmatched}), (V{0, 1}));
    // Connector 1 was on CRTC 1 and stays there though CRTC 0 is free.
    EXPECT_EQ(match_crtcs({0b11, 0b11}, {kUnmatched, 1}), (V{0, 1}));
}

// Connectors that want none (off, disconnected) get none.
TEST(BackendDrm, AScreenThatWantsNoneGetsNone) {
    EXPECT_EQ(match_crtcs({0, 0b1}, {0, kUnmatched}), (V{1, kUnmatched}));
    EXPECT_EQ(match_crtcs({0, 0}, {kUnmatched, kUnmatched}), (V{kUnmatched, kUnmatched}));
}

// More screens than CRTCs: as many as fit.
TEST(BackendDrm, MoreScreensThanCrtcs) {
    const V got = match_crtcs({0b1, 0b1, 0b1}, {kUnmatched});
    ASSERT_EQ(got.size(), 1u);
    EXPECT_NE(got[0], kUnmatched);
}
