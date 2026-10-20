#pragma once
// atrium-record's plain parts, as gpu-screen-recorder has them
// (args_parser.c, recorder/video_codec.c, recorder/codec_select.c,
// recorder/audio_codec.c, color_conversion.c): its options, how a quality
// is a quantizer, which codec a container gets, the audio's head start, the
// video's size, and the colour matrices.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atrium::record {

// gpu-screen-recorder's exit codes (include/recorder/error.h): its front-ends
// tell the user what went wrong by them.
enum Exit : int {
    kOk = 0,
    kFailed = 1,
    kUnsupported = 2,
    kCaptureFailed = 3,
    kCodecQueryFailed = 11,
    kGlFailed = 22,
    kAudioDeviceMissing = 50,
    kMonitorMissing = 51,
    kNoVideoCodec = 52,
    kResolutionUnsupported = 53,
    kCodecUnsupported = 54,
};

enum class Quality { Medium, High, VeryHigh, Ultra };
enum class Codec { Auto, H264, Hevc, Av1, Vp8, Vp9 };
enum class AudioCodec { Opus, Aac, Flac, Mp3 };
enum class FrameRateMode { Vfr, Cfr, Content };
enum class BitrateMode { Auto, Qp, Vbr, Cbr };
enum class Tune { Performance, Quality };
enum class PixelFormat { Yuv420, Yuv444 };

// -k: a codec and how it's carried. hdr implies ten_bit.
struct VideoCodec {
    Codec codec = Codec::Auto;
    bool ten_bit = false;
    bool hdr = false;
    bool vulkan = false;
};

struct Size {
    int width = 0, height = 0;
    bool operator==(const Size&) const = default;
};

struct Region {
    int width = 0, height = 0, x = 0, y = 0;
    bool operator==(const Region&) const = default;
};

// One -a: its devices (split at '|'), and its title from a name: part.
struct AudioTrack {
    std::vector<std::string> sources;  // default_output, default_input, device names
    std::string name;
    bool operator==(const AudioTrack&) const = default;
};

struct Options {
    std::string source;                    // -w: a screen's connector name, "screen", "region"; empty: audio only
    std::string container;                 // -c (mkv is "matroska")
    Size size;                             // -s: at most this size, keeping the aspect ratio (0x0: as captured)
    std::optional<Region> region;          // -region WxH+X+Y, in the desktop's logical pixels
    int fps = 60;                          // -f
    std::vector<AudioTrack> audio;         // -a: one track each
    Quality quality = Quality::VeryHigh;   // -q
    int64_t bitrate = 0;                   // -q with -bm cbr, in bits a second
    VideoCodec codec;                      // -k
    AudioCodec audio_codec = AudioCodec::Opus;  // -ac
    bool audio_codec_given = false;
    int audio_bitrate = 0;                 // -ab, kb/s (0: the codec's own)
    FrameRateMode fm = FrameRateMode::Vfr; // -fm
    BitrateMode bm = BitrateMode::Auto;    // -bm
    PixelFormat pixfmt = PixelFormat::Yuv420;  // -pixfmt
    bool full_range = false;               // -cr full
    Tune tune = Tune::Performance;         // -tune
    bool cursor = true;                    // -cursor
    double keyint = 2.0;                   // -keyint, seconds between keyframes
    bool cpu = false;                      // -encoder cpu
    bool fallback_cpu = false;             // -fallback-cpu-encoding
    std::string file;                      // -o (empty: stdout)
    std::string script;                    // -sc
    std::string ffmpeg_opts, ffmpeg_video_opts, ffmpeg_audio_opts;
    bool verbose = true;                   // -v
    bool gl_debug = false;                 // -gl-debug
    bool low_power = false;                // -low-power
    bool write_first_frame_ts = false;     // -write-first-frame-ts
    bool exclude_metadata = false;         // -exclude-metadata
};

// gpu-screen-recorder's command line. `help`/`version` for -h and --version;
// an error message (and gsr's exit code for it) when it's wrong.
struct Parsed {
    std::optional<Options> options;
    std::string error;
    int exit = kOk;
    bool help = false, version = false;
};
Parsed parse_args(const std::vector<std::string>& args);
extern const char* const kUsage;

// The container's short name: -c, else the file's extension (mkv is
// "matroska" to FFmpeg; the extension as gsr compares it is kept apart).
std::string extension(const std::string& file);

// The H.264 quantizer for a quality (video_quality_to_h264_equivalent_qp),
// the codec's own (AV1/VP9 count in fours, VP8 in twos), and for 10-bit
// HDR 80% of that.
int h264_qp(Quality q);
int codec_qp(Codec c, Quality q, bool hdr);

// "auto" by container: WebM gets VP8; else H.264, HEVC, AV1 in that order,
// the first the GPU encodes at this size.
std::vector<Codec> auto_codecs(const std::string& ext);

// The codec tried after `c` fails (hevc/av1 → h264), none after h264.
std::optional<Codec> fallback_codec(Codec c);

// The audio codec a container takes (select_audio_codec_with_fallback);
// `why` says what changed, when it did.
AudioCodec audio_codec_for(AudioCodec asked, const std::string& ext, bool mixed, std::string* why);
// Its frame size and default bit rate (kb/s).
int audio_frame_size(AudioCodec c);
int audio_bitrate(AudioCodec c);
// How far ahead of the picture the sound is put, in seconds
// (audio_codec_get_desired_delay).
double audio_delay(AudioCodec c, int fps);

// The video's size: the capture's (or a region's), shrunk to fit -s keeping
// its shape, made even, at least 128 a side for the GPU encoders.
Size fit(Size src, Size limit);
Size encoder_size(Size video, bool hardware);

// Frame timing: the clock's frame index, and a frame's pts in the codec's
// time base (µs for vfr, frames for cfr).
int64_t frame_index(int64_t clock_ns, int fps);
int64_t pts_us(int64_t now_ns, int64_t start_ns);

// RGB → YUV, column-major as GLSL's mat4 takes it: BT.709 with
// gpu-screen-recorder's luma weights (0.2110/0.7110/0.0710 full,
// 0.21/0.71/0.07 limited), BT.2020 for HDR. (gsr also uses BT.2020 for
// 10-bit SDR while tagging it BT.709; here 10-bit SDR stays BT.709.)
const float* rgb_to_yuv(bool bt2020, bool full_range);

// The 2x2 map (column-major) from the upright picture to a buffer with
// wl_output transform `t`, about the centre, in normalized coordinates.
void transform_map(int32_t t, float out[4]);
// NV12's luma gain (gsr's 1.007049345); 1 for 10-bit.
float luma_gain(bool ten_bit);

} // namespace atrium::record
