#include "../shell/plugin/Atrium/fingerprint_core.hpp"

#include <gtest/gtest.h>

using namespace atrium;
using A = FingerprintStep::Action;

namespace {

TEST(Fingerprint, AMatchUnlocks) {
    EXPECT_EQ(fingerprint_step("verify-match", true, 3).action, A::Unlock);
}

TEST(Fingerprint, WrongFingersRetryThenGiveUp) {
    int misses = 0;
    for (int i = 1; i < kFingerprintMisses; ++i) {
        const FingerprintStep s = fingerprint_step("verify-no-match", true, misses);
        EXPECT_EQ(s.action, A::Restart) << i;
        EXPECT_EQ(s.misses, i);
        misses = s.misses;
    }
    const FingerprintStep last = fingerprint_step("verify-no-match", true, misses);
    EXPECT_EQ(last.action, A::Stop);
    EXPECT_NE(last.message.find("password"), std::string::npos);
}

TEST(Fingerprint, BadScansDontCountAsWrongFingers) {
    for (const char* r : {"verify-retry-scan", "verify-swipe-too-short", "verify-finger-not-centered",
                          "verify-remove-and-retry"}) {
        const FingerprintStep going = fingerprint_step(r, false, 2);
        EXPECT_EQ(going.action, A::Continue) << r;
        EXPECT_EQ(going.misses, 2) << r;
        EXPECT_FALSE(going.message.empty()) << r;
        // fprintd ended that verify: start another.
        EXPECT_EQ(fingerprint_step(r, true, 2).action, A::Restart) << r;
    }
}

TEST(Fingerprint, ReaderTroubleStops) {
    EXPECT_EQ(fingerprint_step("verify-disconnected", true, 0).action, A::Stop);
    EXPECT_EQ(fingerprint_step("verify-unknown-error", true, 0).action, A::Stop);
    EXPECT_EQ(fingerprint_step("verify-something-new", false, 0).action, A::Stop);
}

} // namespace
