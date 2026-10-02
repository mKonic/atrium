#pragma once
// The PC's end of the audio stream, without PipeWire: packets go in as they
// come off the network (late, twice, out of order or never), and playback
// pulls an even stream of frames out `target` frames behind the newest.
// Lost packets are asked for again (nacks()) while there is time for the
// resend to arrive; the ones that never come are concealed.
//
// The phone's clock and the sound card's drift apart; Drift turns the fill
// level into a playback rate that keeps the buffer at its target.

#include "link_core.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace atrium::phonelink {

struct JitterStats {
    std::uint64_t received = 0;    // packets taken, resends included
    std::uint64_t duplicates = 0;
    std::uint64_t late = 0;        // came after their turn
    std::uint64_t recovered = 0;   // resends that made it in time
    std::uint64_t concealed = 0;   // frames played that never came
    std::uint64_t skipped = 0;     // frames dropped to catch up
    std::uint64_t nacked = 0;      // asks sent
    std::uint64_t resyncs = 0;
};

class JitterBuffer {
public:
    // `target`: frames kept between the newest packet and playback.
    explicit JitterBuffer(std::uint32_t target);

    void setTarget(std::uint32_t target) { target_ = target; }
    std::uint32_t target() const { return target_; }

    // `now` in microseconds, any monotonic clock.
    void push(const AudioPacket& p, std::uint64_t now);
    // Fills `out` (frames * kChannels samples).
    void pull(std::int16_t* out, std::uint32_t frames);
    // The packets to ask for again now.
    std::vector<std::uint32_t> nacks(std::uint64_t now);

    // Frames between the play position and the end of the newest packet;
    // negative when playback ran past it.
    std::int64_t level() const { return synced_ ? newestEnd_ - play_ : 0; }
    bool playing() const { return synced_ && level() > 0; }
    const JitterStats& stats() const { return stats_; }

    // The gap between asks for one packet, and how many asks it gets.
    static constexpr std::uint64_t kNackEvery = 15'000;
    static constexpr int kNackTries = 4;

private:
    struct Missing {
        std::uint64_t since = 0, asked = 0;
        int tries = 0;
    };

    std::int64_t unwrapTs(std::uint32_t ts);
    std::int64_t unwrapSeq(std::uint32_t seq);
    void resync(std::int64_t ts);
    void conceal(std::int16_t* out, std::uint32_t frames);

    std::uint32_t target_;
    bool synced_ = false, haveSeq_ = false;
    std::int64_t play_ = 0, newestEnd_ = 0, start_ = 0;  // start_: the first packet since a resync
    std::uint32_t lastTs32_ = 0, lastSeq32_ = 0;
    std::int64_t lastTs_ = 0, lastSeq_ = 0, highestSeq_ = -1;
    std::map<std::int64_t, std::vector<std::int16_t>> packets_;  // by unwrapped timestamp
    std::map<std::int64_t, Missing> missing_;                    // by unwrapped seq
    // Concealment: the last frames played, faded out when repeated.
    std::vector<std::int16_t> last_;
    std::uint32_t concealPos_ = 0;
    float concealGain_ = 0;
    JitterStats stats_;
};

// Turns the buffer's level into a playback rate (above 1: play faster),
// a PI controller over a smoothed level. Steps are calls to update(), one
// per pulled period.
class Drift {
public:
    // `period`: frames per update.
    double update(std::int64_t level, std::uint32_t target, std::uint32_t period);
    void reset() { *this = {}; }
    double rate() const { return rate_; }

    static constexpr double kMax = 0.005;  // ±0.5%: inaudible

private:
    double smooth_ = 0, integral_ = 0, rate_ = 1;
    bool primed_ = false;
};

} // namespace atrium::phonelink
