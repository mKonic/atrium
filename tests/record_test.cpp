#include "record_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::record;

TEST(Record, Args) {
    std::string why;
    auto o = parse_args({"-o", "DP-1", "-f", "120", "-a", "/tmp/x.mp4"}, &why);
    ASSERT_TRUE(o) << why;
    EXPECT_EQ(o->output, "DP-1");
    EXPECT_EQ(o->fps, 120);
    EXPECT_TRUE(o->audio);
    EXPECT_EQ(o->file, "/tmp/x.mp4");

    o = parse_args({"out.mp4"}, &why);
    ASSERT_TRUE(o);
    EXPECT_EQ(o->output, "");  // the focused screen
    EXPECT_EQ(o->fps, 60);
    EXPECT_FALSE(o->audio);

    EXPECT_FALSE(parse_args({}, &why));
    EXPECT_EQ(why, "which file?");
    EXPECT_FALSE(parse_args({"-f", "0", "x"}, &why));
    EXPECT_FALSE(parse_args({"-f", "6O", "x"}, &why));
    EXPECT_FALSE(parse_args({"-o"}, &why));
    EXPECT_FALSE(parse_args({"-x", "a"}, &why));
    EXPECT_FALSE(parse_args({"a", "b"}, &why));
}

TEST(Record, EncodersGpuFirst) {
    EXPECT_EQ(encoders_to_try({"libx264", "h264_vaapi", "h264_nvenc", "h264_v4l2m2m"}),
              (std::vector<std::string>{"h264_nvenc", "h264_vaapi", "libx264"}));
    EXPECT_EQ(encoders_to_try({"libx264"}), std::vector<std::string>{"libx264"});
    EXPECT_TRUE(encoders_to_try({"libopenh264"}).empty());
}

TEST(Record, FramesCappedAtTheRate) {
    EXPECT_TRUE(keep_frame(0, -1, 60));
    EXPECT_TRUE(keep_frame(16'667, 0, 60));     // 60 Hz screen at 60: all
    EXPECT_TRUE(keep_frame(16'000, 0, 60));     // a little early: still
    EXPECT_FALSE(keep_frame(5'556, 0, 60));     // 180 Hz screen at 60: every third
    EXPECT_FALSE(keep_frame(11'111, 0, 60));
    EXPECT_TRUE(keep_frame(16'667, 0, 60));
}

TEST(Record, ClockStartsAtTheFirstFrame) {
    Timeline t;
    EXPECT_FALSE(t.started());
    EXPECT_FALSE(t.audio_us(1'000'000));  // sound before any frame: nowhere yet
    EXPECT_EQ(t.video_us(5'000'000'000), 0);
    EXPECT_TRUE(t.started());
    EXPECT_EQ(t.video_us(5'016'000'000), 16'000);
    EXPECT_EQ(t.video_us(5'010'000'000), 16'001);  // a stamp going back: still after the last
    EXPECT_EQ(*t.audio_us(5'100'000'000), 100'000);
    EXPECT_EQ(*t.audio_us(4'990'000'000), -10'000);  // just before the first frame
}
