#pragma once
// Network throughput from /proc/net/dev, as plain C++ so it is tested
// without Qt; netspeed.cpp samples it for QML.

#include <cstdint>
#include <string>
#include <string_view>

namespace atrium::netspeed {

struct Totals {
    uint64_t rx = 0, tx = 0;
};

// Bytes received and sent so far over the real interfaces: loopback and
// virtual ones (bridges, veths, tunnels to containers) would count twice.
Totals read_totals(std::string_view proc_net_dev);

// Whether an interface is one traffic really leaves the machine through.
bool counts(std::string_view name);

// A rate that reads steadily: an exponential average whose weight comes from
// the time that really passed, so a late sample counts for proportionally
// more. Raw per-second deltas jump by a factor of two on a steady transfer.
class Smoother {
public:
    // Time to cover ~63% of a step change: long enough to flatten the
    // per-second noise, short enough that a stall still shows within seconds.
    static constexpr double kSeconds = 3.0;

    // The first sample seeds the average instead of climbing out of zero.
    double add(double instant, double seconds);
    void reset() { seeded_ = false, rate_ = 0; }
    double rate() const { return rate_; }

private:
    double rate_ = 0;
    bool seeded_ = false;
};

// "0 KB/s", "340 KB/s", "1.2 MB/s", "12 MB/s", "1.1 GB/s"
std::string format_rate(double bytes_per_second);

} // namespace atrium::netspeed
