#include "netspeed_core.hpp"

#include <cmath>
#include <cstdio>

namespace atrium::netspeed {

bool counts(std::string_view name) {
    for (std::string_view skip : {"lo", "docker", "br-", "veth", "virbr", "vnet", "tun", "tap", "wg", "tailscale",
                                  "zt", "podman", "cni", "flannel", "vmnet", "vboxnet"})
        if (name.starts_with(skip))
            return false;
    return !name.empty();
}

Totals read_totals(std::string_view text) {
    Totals t;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;  // the two header lines
        std::string_view name = line.substr(0, colon);
        while (!name.empty() && name.front() == ' ')
            name.remove_prefix(1);
        if (!counts(name))
            continue;
        // rx: bytes packets errs drop fifo frame compressed multicast; then tx: bytes ...
        unsigned long long v[9] = {};
        const std::string rest(line.substr(colon + 1));
        if (std::sscanf(rest.c_str(), "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6], &v[7], &v[8]) == 9) {
            t.rx += v[0];
            t.tx += v[8];
        }
    }
    return t;
}

std::string format_rate(double bps) {
    const char* units[] = {"KB/s", "MB/s", "GB/s"};
    double v = std::max(0.0, bps) / 1000.0;
    int u = 0;
    while (v >= 1000.0 && u < 2) {
        v /= 1000.0;
        ++u;
    }
    char out[32];
    if (u == 0 || v >= 10)
        std::snprintf(out, sizeof out, "%.0f %s", std::floor(v + 0.5), units[u]);
    else
        std::snprintf(out, sizeof out, "%.1f %s", v, units[u]);
    return out;
}

} // namespace atrium::netspeed
