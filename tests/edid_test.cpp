#include "edid.hpp"
#include "paths.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

using namespace atrium;

namespace {

std::vector<uint8_t> read(const char* name) {
    std::ifstream f(std::string(ATRIUM_SOURCE_DIR) + "/tests/data/" + name, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(Edid, HdrScreenSaysItTakesHdr10) {
    // MSI G274F: HDR10 at ~400 nits.
    auto caps = hdr_caps_from_edid(read("msi-g274f.edid"));
    ASSERT_TRUE(caps);
    EXPECT_TRUE(caps->pq);
    EXPECT_TRUE(caps->bt2020);
    EXPECT_NEAR(caps->max_nits, 400.0, 0.5);
    EXPECT_NEAR(caps->max_frame_avg_nits, 282.8, 0.5);
}

TEST(Edid, NotAnEdid) {
    const std::vector<uint8_t> junk(128, 0x42);
    EXPECT_FALSE(hdr_caps_from_edid(junk));
    EXPECT_FALSE(hdr_caps_from_edid({}));
}
