#pragma once
// atrium-record's decisions, apart from libav and PipeWire so they can be
// tested: what was asked for, which encoder to use, and where frames and
// sound samples fall in time.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atrium::record {

struct Options {
    std::string output;  // the screen; empty: the focused one
    int fps = 60;
    bool audio = false;  // what the speakers play
    std::string file;
};

// atrium-record [-o SCREEN] [-f FPS] [-a] FILE; nullopt (and why) if wrong.
std::optional<Options> parse_args(const std::vector<std::string>& args, std::string* error);

// The H.264 encoders to try, in order, from those this libav has: the
// GPU's own first (NVENC, then VA-API), x264 in software last. One that
// won't open (NVENC without an NVIDIA card) gives way to the next.
std::vector<std::string> encoders_to_try(const std::vector<std::string>& available);

// Frames at most `fps` a second: whether one at `us` is kept after one kept
// at `last_us` (-1: none yet). A little early is allowed, or a screen at
// exactly that rate would lose every other frame to jitter.
bool keep_frame(int64_t us, int64_t last_us, int fps);

// A clock that starts with the first frame: times in ns (CLOCK_MONOTONIC,
// as PipeWire stamps buffers) to microseconds since then. Sound that came
// before the first frame is dropped (negative).
class Timeline {
public:
    // A video frame's time; the first sets zero. Never earlier than the last.
    int64_t video_us(int64_t ns);
    // Sound captured at `ns`, relative to the first frame (negative before
    // it); nullopt until there is one.
    std::optional<int64_t> audio_us(int64_t ns) const;
    bool started() const { return start_ns_ >= 0; }

private:
    int64_t start_ns_ = -1;
    int64_t last_us_ = -1;
};

} // namespace atrium::record
