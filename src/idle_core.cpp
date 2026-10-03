#include "idle_core.hpp"

#include <charconv>
#include <string>

namespace atrium {

int idle_after_seconds(std::string_view choice) {
    const size_t dash = choice.find('-');
    if (dash == std::string_view::npos)
        return 0;
    int n = 0;
    auto [p, ec] = std::from_chars(choice.data(), choice.data() + dash, n);
    if (ec != std::errc() || p != choice.data() + dash || n <= 0)
        return 0;
    const std::string_view unit = choice.substr(dash + 1);
    if (unit == "minute" || unit == "minutes")
        return n * 60;
    if (unit == "hour" || unit == "hours")
        return n * 3600;
    return 0;
}

IdleDue idle_due(const IdleTimes& times, int64_t idle_ms, uint32_t done) {
    IdleDue due;
    for (int i = 0; i < int(IdleStage::Count); ++i) {
        const int s = times.seconds[i];
        if (s <= 0 || (done & (1u << i)))
            continue;
        const int64_t at = int64_t(s) * 1000;
        if (idle_ms >= at)
            due.now |= 1u << i;
        else if (due.next_ms < 0 || at - idle_ms < due.next_ms)
            due.next_ms = at - idle_ms;
    }
    return due;
}

} // namespace atrium
