#include "edid.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

extern "C" {
#include <libdisplay-info/info.h>
}

namespace atrium {

std::optional<HdrCaps> hdr_caps_from_edid(std::span<const uint8_t> edid) {
    if (edid.empty())
        return std::nullopt;
    di_info* info = di_info_parse_edid(edid.data(), edid.size());
    if (!info)
        return std::nullopt;
    HdrCaps caps;
    const di_hdr_static_metadata* hdr = di_info_get_hdr_static_metadata(info);
    caps.pq = hdr->type1 && hdr->pq;
    caps.max_nits = hdr->desired_content_max_luminance;
    caps.max_frame_avg_nits = hdr->desired_content_max_frame_avg_luminance;
    caps.min_nits = hdr->desired_content_min_luminance;
    const di_supported_signal_colorimetry* c = di_info_get_supported_signal_colorimetry(info);
    caps.bt2020 = c->bt2020_rgb;
    di_info_destroy(info);
    return caps;
}

std::vector<uint8_t> connector_edid(const std::string& connector) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        const std::string name = entry.path().filename();
        // cardN-DP-1: the part after the first dash is the connector.
        const auto dash = name.find('-');
        if (name.rfind("card", 0) != 0 || dash == std::string::npos || name.substr(dash + 1) != connector)
            continue;
        std::ifstream status(entry.path() / "status");
        std::string s;
        if (!(status >> s) || s != "connected")
            continue;
        std::ifstream f(entry.path() / "edid", std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        if (!bytes.empty())
            return bytes;
    }
    return {};
}

} // namespace atrium
