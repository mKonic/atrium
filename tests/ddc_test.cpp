#include "ddc_core.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace atrium::ddc;

namespace {

// The MSI G274F's answer to Get VCP 0x10 at 100 of 100, as ddcutil traced it.
const std::vector<uint8_t> kReply = {0x6e, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x64, 0xa4};

} // namespace

TEST(Ddc, RequestsCarryTheirChecksum) {
    // ddcutil's trace of `getvcp 10`.
    EXPECT_EQ(get_request(kBrightness), (std::array<uint8_t, 5>{0x51, 0x82, 0x01, 0x10, 0xac}));
    // 0x6E ^ 51 ^ 84 ^ 03 ^ 10 ^ 00 ^ 32.
    EXPECT_EQ(set_request(kBrightness, 50), (std::array<uint8_t, 7>{0x51, 0x84, 0x03, 0x10, 0x00, 0x32, 0x9a}));
}

TEST(Ddc, ReadsAReplyAndWhatFollowsIt) {
    std::vector<uint8_t> read = kReply;
    read.insert(read.end(), kReply.begin(), kReply.end());  // the monitor repeats it
    Vcp vcp;
    ASSERT_EQ(parse_get_reply(read, kBrightness, vcp), Reply::Ok);
    EXPECT_EQ(vcp.current, 100);
    EXPECT_EQ(vcp.max, 100);
}

TEST(Ddc, SkipsADoubledSourceAddress) {
    std::vector<uint8_t> read = {0x6e};
    read.insert(read.end(), kReply.begin(), kReply.end());
    Vcp vcp;
    EXPECT_EQ(parse_get_reply(read, kBrightness, vcp), Reply::Ok);
}

TEST(Ddc, TellsNullAndUnsupportedFromGarbage) {
    Vcp vcp;
    EXPECT_EQ(parse_get_reply(std::vector<uint8_t>{0x6e, 0x80, 0xbe}, kBrightness, vcp), Reply::Null);
    // Result code 1: no such feature (checksum moves with it).
    std::vector<uint8_t> unsupported = kReply;
    unsupported[3] = 0x01;
    unsupported[10] ^= 0x01;
    EXPECT_EQ(parse_get_reply(unsupported, kBrightness, vcp), Reply::Unsupported);
    // What NVIDIA's adapter gave back read too early or too short.
    EXPECT_EQ(parse_get_reply(std::vector<uint8_t>(12, 0x6e), kBrightness, vcp), Reply::Garbled);
    EXPECT_EQ(parse_get_reply(std::vector<uint8_t>{0x51, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
                              kBrightness, vcp),
              Reply::Garbled);
    EXPECT_EQ(parse_get_reply(std::vector<uint8_t>(40, 0xff), kBrightness, vcp), Reply::Garbled);
    // One bit off fails the checksum; another feature's answer isn't this one's.
    std::vector<uint8_t> flipped = kReply;
    flipped[9] ^= 0x01;
    EXPECT_EQ(parse_get_reply(flipped, kBrightness, vcp), Reply::Garbled);
    EXPECT_EQ(parse_get_reply(kReply, 0x12, vcp), Reply::Garbled);
}

TEST(Ddc, ProbesDisplayBusesOnly) {
    EXPECT_FALSE(ignorable_bus("NVIDIA i2c adapter 4 at 1:00.0", "nvidia", 0x030000));
    EXPECT_FALSE(ignorable_bus("AMDGPU DM i2c hw bus 1", "amdgpu", 0x030000));
    EXPECT_FALSE(ignorable_bus("i915 gmbus dpb", "i915", 0x030000));
    EXPECT_FALSE(ignorable_bus("DPMST", "", 0));
    EXPECT_FALSE(ignorable_bus("dock adapter", "", 0x0a0000));
    EXPECT_TRUE(ignorable_bus("Synopsys DesignWare I2C adapter", "i2c_designware", 0));
    EXPECT_TRUE(ignorable_bus("SMBus PIIX4 adapter port 0 at 0b00", "piix4_smbus", 0x0c0500));
    EXPECT_TRUE(ignorable_bus("AMDGPU SMU 0", "amdgpu", 0x030000));
    EXPECT_TRUE(ignorable_bus("nouveau-0000:01:00.0-aux", "nouveau", 0x030000));
    EXPECT_FALSE(ignorable_bus("nvkm-0000:01:00.0-bus-0005", "nouveau", 0x030000));
    EXPECT_TRUE(ignorable_bus("some sensor", "", 0x0c0500));
    EXPECT_TRUE(ignorable_bus("", "", 0));
}

TEST(Ddc, KnowsAnEdid) {
    EXPECT_TRUE(edid_header(std::vector<uint8_t>{0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x36}));
    EXPECT_FALSE(edid_header(std::vector<uint8_t>(128, 0xff)));
    EXPECT_FALSE(edid_header(std::vector<uint8_t>{0x00, 0xff}));
}
