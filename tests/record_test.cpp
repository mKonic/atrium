#include "record_core.hpp"

#include <gtest/gtest.h>


using namespace atrium::record;

TEST(Record, GpuScreenRecordersOptions) {
    // As atrium's shell starts it.
    const auto p = parse_args({"-w", "DP-1", "-f", "180", "-o", "/tmp/a.mp4", "-a", "default_output"});
    ASSERT_TRUE(p.options) << p.error;
    EXPECT_EQ(p.options->source, "DP-1");
    EXPECT_EQ(p.options->fps, 180);
    EXPECT_EQ(p.options->file, "/tmp/a.mp4");
    ASSERT_EQ(p.options->audio.size(), 1u);
    EXPECT_EQ(p.options->audio[0].sources, std::vector<std::string>{"default_output"});
    EXPECT_EQ(p.options->quality, Quality::VeryHigh);  // gpu-screen-recorder's defaults
    EXPECT_EQ(p.options->codec.codec, Codec::Auto);
    EXPECT_EQ(p.options->fm, FrameRateMode::Vfr);
    EXPECT_EQ(p.options->bm, BitrateMode::Qp);  // auto
    EXPECT_FALSE(p.options->full_range);
    EXPECT_TRUE(p.options->cursor);

    const auto q = parse_args({"-w", "HDMI-A-1", "-o", "x.mkv", "-k", "hevc_hdr", "-q", "medium", "-cursor", "no", "-c", "mkv",
                               "-a", "default_output|device:alsa_input.usb|name:Game", "-a", "default_input", "-fm", "cfr",
                               "-region", "640x480+10+20", "-s", "1280x720"});
    ASSERT_FALSE(q.options) << "a region needs -w region";
    const auto r = parse_args({"-w", "region", "-o", "x.mkv", "-k", "hevc_hdr", "-q", "medium", "-cursor", "no", "-c", "mkv",
                               "-a", "default_output|device:alsa_input.usb|name:Game", "-a", "default_input", "-fm", "cfr",
                               "-region", "640x480+10+20", "-s", "1280x720"});
    ASSERT_TRUE(r.options) << r.error;
    const Options& o = *r.options;
    EXPECT_EQ(o.codec.codec, Codec::Hevc);
    EXPECT_TRUE(o.codec.hdr && o.codec.ten_bit);
    EXPECT_EQ(o.quality, Quality::Medium);
    EXPECT_FALSE(o.cursor);
    EXPECT_EQ(o.container, "matroska");
    ASSERT_EQ(o.audio.size(), 2u);
    EXPECT_EQ(o.audio[0].sources, (std::vector<std::string>{"default_output", "alsa_input.usb"}));
    EXPECT_EQ(o.audio[0].name, "Game");
    EXPECT_EQ(o.fm, FrameRateMode::Cfr);
    EXPECT_EQ(*o.region, (Region{640, 480, 10, 20}));
    EXPECT_EQ(o.size, (Size{1280, 720}));

    const auto cbr = parse_args({"-w", "screen", "-o", "x.mp4", "-bm", "cbr", "-q", "8000", "-k", "av1_10bit_vulkan"});
    ASSERT_TRUE(cbr.options) << cbr.error;
    EXPECT_EQ(cbr.options->bitrate, 8000000);
    EXPECT_TRUE(cbr.options->codec.vulkan && cbr.options->codec.ten_bit && !cbr.options->codec.hdr);
    EXPECT_EQ(cbr.options->codec.codec, Codec::Av1);

    // Audio only, to stdout with a container.
    EXPECT_TRUE(parse_args({"-a", "default_input", "-c", "mp4"}).options);
}

TEST(Record, WrongOptionsSayWhatAndExitAsGsr) {
    EXPECT_FALSE(parse_args({"-w", "DP-1"}).options);                         // stdout needs -c
    EXPECT_FALSE(parse_args({"-o", "x.mp4"}).options);                        // nothing to record
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-f", "0"}).options);
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-k", "vp7"}).options);
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o"}).options);                   // no value
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-w", "DP-2"}).options);  // twice
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-cursor", "maybe"}).options);
    EXPECT_FALSE(parse_args({"-w", "DP-1", "-o", "x.mp4", "-bm", "cbr"}).options);  // cbr needs -q kbps
    const auto bad = parse_args({"-w", "DP-1", "-o", "x.mp4", "-x", "1"});
    EXPECT_NE(bad.error.find("-x"), std::string::npos);
    EXPECT_EQ(bad.exit, kFailed);
    // gsr's that aren't here yet: unsupported, not a typo.
    EXPECT_EQ(parse_args({"-w", "DP-1", "-o", "dir", "-r", "30", "-c", "mp4"}).exit, kUnsupported);
    EXPECT_EQ(parse_args({"-w", "portal", "-o", "x.mp4"}).exit, kUnsupported);
    EXPECT_EQ(parse_args({"-w", "DP-1", "-o", "x.mp4", "-a", "app:firefox"}).exit, kUnsupported);
    EXPECT_EQ(parse_args({"-w", "DP-1", "-o", "rtmp://example/live"}).exit, kUnsupported);
    EXPECT_TRUE(parse_args({"--help"}).help);
    EXPECT_TRUE(parse_args({"--version"}).version);
}

TEST(Record, QualityAsAQuantizer) {
    EXPECT_EQ(h264_qp(Quality::Medium), 35);
    EXPECT_EQ(h264_qp(Quality::VeryHigh), 25);
    EXPECT_EQ(codec_qp(Codec::Hevc, Quality::High, false), 30);
    EXPECT_EQ(codec_qp(Codec::Av1, Quality::VeryHigh, false), 100);
    EXPECT_EQ(codec_qp(Codec::Vp8, Quality::Ultra, false), 44);
    EXPECT_EQ(codec_qp(Codec::Hevc, Quality::VeryHigh, true), 20);  // 10-bit: 80%
}

TEST(Record, CodecsByContainerAndFallback) {
    EXPECT_EQ(auto_codecs("webm"), std::vector<Codec>{Codec::Vp8});
    EXPECT_EQ(auto_codecs("mp4"), (std::vector<Codec>{Codec::H264, Codec::Hevc, Codec::Av1}));
    EXPECT_EQ(fallback_codec(Codec::Av1), Codec::H264);
    EXPECT_FALSE(fallback_codec(Codec::H264));
    std::string why;
    EXPECT_EQ(audio_codec_for(AudioCodec::Opus, "mp4", false, &why), AudioCodec::Opus);
    EXPECT_EQ(audio_codec_for(AudioCodec::Opus, "mov", false, &why), AudioCodec::Aac);
    EXPECT_FALSE(why.empty());
    EXPECT_EQ(audio_codec_for(AudioCodec::Aac, "webm", false, &why), AudioCodec::Opus);
    EXPECT_EQ(audio_codec_for(AudioCodec::Opus, "mp3", false, &why), AudioCodec::Mp3);
    EXPECT_EQ(extension("/a.b/video.mkv"), "mkv");
    EXPECT_EQ(extension("/a.b/video"), "");
}

TEST(Record, SoundAheadByTheCodecsDelay) {
    EXPECT_NEAR(audio_delay(AudioCodec::Opus, 60), 0.01 + 1.0 / 165 - 1.0 / 60, 1e-9 + 1.0);  // clamped at 0
    EXPECT_DOUBLE_EQ(audio_delay(AudioCodec::Opus, 60), 0.0);
    EXPECT_NEAR(audio_delay(AudioCodec::Aac, 60), (0.01 + 1.0 / 165 + 0.008) * 2 - 1.0 / 60, 1e-9);
    EXPECT_EQ(audio_frame_size(AudioCodec::Aac), 1024);
    EXPECT_EQ(audio_bitrate(AudioCodec::Opus), 128);
}

TEST(Record, VideoSize) {
    EXPECT_EQ(fit({2560, 1440}, {1280, 1280}), (Size{1280, 720}));
    EXPECT_EQ(fit({1920, 1080}, {}), (Size{1920, 1080}));
    EXPECT_EQ(encoder_size({1919, 1079}, true), (Size{1920, 1080}));
    EXPECT_EQ(encoder_size({100, 51}, true), (Size{128, 128}));
    EXPECT_EQ(encoder_size({100, 51}, false), (Size{100, 52}));
}

TEST(Record, Clock) {
    EXPECT_EQ(frame_index(999'999'999, 60), 59);
    EXPECT_EQ(frame_index(1'000'000'000, 60), 60);
    EXPECT_EQ(pts_us(2'500'000'000, 1'000'000'000), 1'500'000);
    EXPECT_EQ(pts_us(1, 5), 0);
}

TEST(Record, ColourMatricesAsGsrs) {
    // Column-major: m[col*4 + row]. White is Y=1 full, 235/255 limited.
    const float* full = rgb_to_yuv(false, true);
    EXPECT_NEAR(full[0], 0.2110f, 1e-6);
    EXPECT_NEAR(full[4], 0.7110f, 1e-6);
    EXPECT_NEAR(full[1], -0.113563f, 1e-5);  // gsr's RGB_TO_NV12_FULL
    EXPECT_NEAR(full[6], -0.450570f, 1e-5);
    const float* lim = rgb_to_yuv(false, false);
    EXPECT_NEAR(lim[0], 0.180353f, 1e-5);    // gsr's RGB_TO_NV12_LIMITED
    EXPECT_NEAR(lim[1], -0.096964f, 1e-5);
    EXPECT_NEAR(lim[12], 0.062745f, 1e-5);
    const float* p010 = rgb_to_yuv(true, false);
    EXPECT_NEAR(p010[0], 0.225613f, 1e-5);   // gsr's RGB_TO_P010_LIMITED
    EXPECT_NEAR(p010[2], 0.429412f, 1e-5);
    EXPECT_FLOAT_EQ(luma_gain(false), 1.007049345f);
}

TEST(Record, BufferTransforms) {
    float m[4];
    transform_map(0, m);
    EXPECT_EQ(std::vector<float>(m, m + 4), (std::vector<float>{1, 0, 0, 1}));
    // 90: wlroots' {0, 1, -1, 0} row-major, column-major here.
    transform_map(1, m);
    EXPECT_EQ(std::vector<float>(m, m + 4), (std::vector<float>{0, -1, 1, 0}));
}
