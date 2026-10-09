#include "record_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::record;

TEST(Record, GpuScreenRecordersOptions) {
    // As atrium's shell starts it.
    const auto p = parse_args({"-w", "DP-1", "-f", "180", "-o", "/tmp/a.mp4", "-a", "default_output"});
    ASSERT_TRUE(p.options) << p.error;
    EXPECT_EQ(p.options->output, "DP-1");
    EXPECT_EQ(p.options->fps, 180);
    EXPECT_EQ(p.options->file, "/tmp/a.mp4");
    EXPECT_TRUE(p.options->audio);
    EXPECT_EQ(p.options->quality, Quality::VeryHigh);  // gpu-screen-recorder's default
    EXPECT_TRUE(p.options->cursor);

    const auto q = parse_args({"-w", "HDMI-A-1", "-o", "x.mp4", "-k", "hevc", "-q", "medium", "-cursor", "no"});
    ASSERT_TRUE(q.options);
    EXPECT_EQ(q.options->codec, Codec::Hevc);
    EXPECT_EQ(q.options->quality, Quality::Medium);
    EXPECT_FALSE(q.options->cursor);
    EXPECT_FALSE(q.options->audio);
}

TEST(Record, WrongOptionsSaySo) {
    EXPECT_FALSE(parse_args({"-o", "x.mp4"}).options);                          // no screen
    EXPECT_FALSE(parse_args({"-w", "DP-1"}).options);                           // no file
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-f", "0"}).options);
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-a", "mic"}).options);
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-k", "vp8"}).options);
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o"}).options);                     // no value
    EXPECT_NE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-x", "1"}).error.find("-x"), std::string::npos);
}

TEST(Record, QualityAsAQuantizer) {
    EXPECT_EQ(h264_qp(Quality::Medium), 35);
    EXPECT_EQ(h264_qp(Quality::VeryHigh), 25);
    EXPECT_EQ(codec_qp(Codec::Hevc, Quality::High), 30);
    EXPECT_EQ(codec_qp(Codec::Av1, Quality::VeryHigh), 100);
}

TEST(Record, EncodersBestFirst) {
    EXPECT_EQ(encoders(Codec::H264, true), (std::vector<std::string>{"h264_nvenc", "h264_vaapi", "libx264"}));
    EXPECT_EQ(encoders(Codec::Hevc, false), (std::vector<std::string>{"hevc_vaapi", "libx264"}));
}

TEST(Record, Timestamps) {
    EXPECT_EQ(pts_us(2'500'000'000, 1'000'000'000), 1'500'000);
    EXPECT_EQ(pts_us(1, 5), 0);  // before the start: at it
}
