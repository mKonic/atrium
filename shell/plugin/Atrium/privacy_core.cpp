#include "privacy_core.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace atrium::privacy {

std::vector<std::string> microphone_apps(const std::vector<Capture>& streams) {
    std::vector<std::string> out;
    for (const Capture& c : streams) {
        if (c.monitor || c.corked || c.media_name == "Peak detect" || c.app == "atrium")
            continue;
        out.push_back(c.app.empty() ? "An app" : c.app);
    }
    return out;
}

bool is_camera(const std::string& path) {
    constexpr std::string_view prefix = "/dev/video";
    if (!path.starts_with(prefix) || path.size() == prefix.size())
        return false;
    return std::all_of(path.begin() + long(prefix.size()), path.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string display_name(const std::string& comm) {
    if (comm == "pipewire" || comm == "wireplumber")
        return "An app (through PipeWire)";
    return comm.empty() ? "An app" : comm;
}

std::vector<std::string> camera_apps(const std::map<int, std::vector<std::string>>& fds,
                                     const std::map<int, std::string>& names) {
    std::vector<std::string> out;
    for (const auto& [pid, targets] : fds) {
        if (std::none_of(targets.begin(), targets.end(), is_camera))
            continue;
        const auto it = names.find(pid);
        out.push_back(display_name(it == names.end() ? "" : it->second));
    }
    return out;
}

std::vector<Use> merge(std::vector<Use> uses) {
    std::stable_sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) { return int(a.kind) < int(b.kind); });
    std::vector<Use> out;
    for (const Use& u : uses)
        if (std::find(out.begin(), out.end(), u) == out.end())
            out.push_back(u);
    return out;
}

} // namespace atrium::privacy
