#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace atrium {

// What a screen's EDID says about HDR: the luminances an HDR10 signal's
// metadata describes the screen with (as Windows fills it from the EDID).
struct HdrCaps {
    bool pq = false;                 // takes SMPTE ST 2084
    bool bt2020 = false;             // takes BT.2020 RGB
    double max_nits = 0;             // desired content max luminance, 0 if unset
    double max_frame_avg_nits = 0;   // desired content max frame-average luminance
    double min_nits = 0;             // desired content min luminance
};

std::optional<HdrCaps> hdr_caps_from_edid(std::span<const uint8_t> edid);

// The EDID the kernel read for a connector ("DP-1"), from sysfs; empty if
// there is none (nested, headless, or not connected).
std::vector<uint8_t> connector_edid(const std::string& connector);

} // namespace atrium
