#include "updates_core.hpp"

#include <array>

namespace atrium::updates {

PackageId parse_package_id(std::string_view id) {
    std::array<std::string_view, 4> parts;
    for (size_t i = 0; i < parts.size(); ++i) {
        const size_t semi = id.find(';');
        if (semi == std::string_view::npos && i < parts.size() - 1)
            return {};
        parts[i] = id.substr(0, semi);
        id.remove_prefix(semi == std::string_view::npos ? id.size() : semi + 1);
    }
    if (parts[0].empty())
        return {};
    return {std::string(parts[0]), std::string(parts[1]), std::string(parts[2]), std::string(parts[3])};
}

std::string doing(unsigned info) {
    switch (info) {
    case pk::kInfoDownloading: return "Downloading";
    case pk::kInfoPreparing: return "Preparing";
    case pk::kInfoDecompressing: return "Unpacking";
    case pk::kInfoUpdating:
    case pk::kInfoInstalling: return "Installing";
    case pk::kInfoRemoving: return "Removing";
    case pk::kInfoCleanup: return "Cleaning up";
    default: return "";
    }
}

bool needs_restart(std::string_view name) {
    static constexpr std::string_view whole[] = {"systemd", "glibc", "linux-firmware", "mesa", "dbus",
                                                 "dbus-broker", "amd-ucode", "intel-ucode"};
    for (std::string_view w : whole)
        if (name == w)
            return true;
    // linux, linux-lts, linux-cachyos, linux-zen …; nvidia, nvidia-open, nvidia-utils …
    const bool kernel = (name == "linux" || name.starts_with("linux-")) && !name.starts_with("linux-api") &&
                        !name.starts_with("linux-tools") && !name.starts_with("linux-firmware-");
    return kernel || name.starts_with("nvidia");
}

std::string summary(int count, int security) {
    if (count <= 0)
        return "No updates";
    std::string s = std::to_string(count) + (count == 1 ? " update" : " updates");
    if (security > 0)
        s += ", " + std::to_string(security) + " for security";
    return s;
}

} // namespace atrium::updates
