#pragma once
// atrium-record's plain parts, as gpu-screen-recorder has them
// (args_parser.c, recorder/video_codec.c): its options (the ones atrium
// uses take the same form), how a quality is a quantizer, and which encoder
// to try first.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atrium::record {

enum class Quality { Medium, High, VeryHigh, Ultra };
enum class Codec { H264, Hevc, Av1 };

struct Options {
    std::string output;        // -w: the screen's connector name
    int fps = 60;              // -f
    std::string file;          // -o
    bool audio = false;        // -a default_output
    Codec codec = Codec::H264; // -k h264|hevc|av1 (auto: h264)
    Quality quality = Quality::VeryHigh;  // -q medium|high|very_high|ultra
    bool cursor = true;        // -cursor yes|no
};

// gpu-screen-recorder's command line; an error message when it's wrong.
struct Parsed {
    std::optional<Options> options;
    std::string error;
};
Parsed parse_args(const std::vector<std::string>& args);

// The H.264 quantizer for a quality (video_quality_to_h264_equivalent_qp),
// and the codec's own (AV1 counts in fours).
int h264_qp(Quality q);
int codec_qp(Codec c, Quality q);

// Encoders to try, best first: NVIDIA's, then VA-API's, then the CPU's.
std::vector<std::string> encoders(Codec c, bool nvidia);

// Microseconds since the recording began.
int64_t pts_us(int64_t now_ns, int64_t start_ns);

} // namespace atrium::record
