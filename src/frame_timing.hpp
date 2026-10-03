#pragma once
// How long compositing a frame takes, and so how long before a vblank it
// must start (Output::frame). Modelled on KWin's RenderJournal: the
// estimate jumps up to a slow frame at once and comes back down slowly, so
// one cheap frame doesn't make the next expensive one miss.

#include <cstdint>

namespace atrium::frame_timing {

class RenderJournal {
public:
    // A frame took `ns` from starting to composite until the GPU finished.
    void add(int64_t ns);
    // What the next frame is likely to take (0 before any).
    int64_t estimate() const { return estimate_; }

private:
    int64_t estimate_ = 0;
};

// How long before the vblank compositing starts: the estimate, plus
// `slack` for the commit and the scheduler, at least `min` and at most half
// the refresh `period`.
int64_t margin(int64_t estimate, int64_t slack, int64_t min, int64_t period);

// When the GPU finished: the signal time of a sync_file's fence(s),
// CLOCK_MONOTONIC ns; 0 if not yet signalled or it can't tell.
int64_t fence_signalled_ns(int sync_file_fd);

} // namespace atrium::frame_timing
