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

// "0 KB/s", "340 KB/s", "1.2 MB/s", "12 MB/s", "1.1 GB/s"
std::string format_rate(double bytes_per_second);

} // namespace atrium::netspeed
