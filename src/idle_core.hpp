#pragma once
// When atrium acts on an idle session: the settings' "after" choices, and
// which of the stages is due.
#include <cstdint>
#include <string_view>

namespace atrium {

// "never", "1-minute", "5-minutes", "1-hour", ...: the choices a Power
// setting offers. Seconds; 0 for never (or anything unknown).
int idle_after_seconds(std::string_view choice);

enum class IdleStage : uint8_t { Dim, ScreenOff, Lock, Suspend, Count };

struct IdleTimes {
    int seconds[int(IdleStage::Count)] = {};  // 0: never
};

struct IdleDue {
    // Stages to act on now (bits by IdleStage), none acted on yet this time.
    uint32_t now = 0;
    // Milliseconds until the next one is due; -1 when nothing is left.
    int64_t next_ms = -1;
};

// `idle_ms` since the last input; `done`: stages already acted on.
IdleDue idle_due(const IdleTimes& times, int64_t idle_ms, uint32_t done);

} // namespace atrium
