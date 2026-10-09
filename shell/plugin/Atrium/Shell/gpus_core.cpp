#include "gpus_core.hpp"

#include <drm/amdgpu_drm.h>
#include <drm/xe_drm.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

namespace atrium::gpus {

namespace fs = std::filesystem;

namespace {

// switcheroo's get_vk_driver_match.
const char* vk_driver_match(const std::string& driver) {
    static const std::pair<const char*, const char*> kMatches[] = {
        {"amdgpu", "*radeon*"}, {"i915", "*intel*"}, {"nvidia", "*nvidia*"}, {"radeon", "*radeon*"}, {"xe", "*intel*"},
    };
    for (const auto& [kernel, match] : kMatches)
        if (driver == kernel)
            return match;
    return nullptr;
}

// info-cleanup.c's prettify_info and remove_duplicate_whitespace.
std::string cleanup(std::string s) {
    static const std::pair<std::regex, const char*> kRules[] = {
        {std::regex("Mesa DRI "), ""},
        {std::regex("Mesa Intel"), "Intel"},
        {std::regex("[(]R[)]"), "®"},
        {std::regex("[(](tm|TM)[)]"), "™"},
        {std::regex("(ATI|EPYC|AMD FX|Radeon|Ryzen|Threadripper|GeForce RTX) "), "$1™ "},
        {std::regex("Gallium \\d+\\.\\d+ on (.*)"), "$1"},
        {std::regex(" CPU| Processor| \\S+-Core| @ \\d+\\.\\d+GHz"), ""},
        {std::regex(" x86|/MMX|/SSE2|/PCIe"), ""},
        {std::regex(" [(][^)]*(DRM|MESA|LLVM)[^)]*[)]?"), ""},
        {std::regex("Graphics Controller"), "Graphics"},
        {std::regex(".*llvmpipe.*"), "Software Rendering"},
    };
    const auto first = s.find_first_not_of(" \t\n\r"), last = s.find_last_not_of(" \t\n\r");
    s = first == std::string::npos ? "" : s.substr(first, last - first + 1);
    for (const auto& [re, with] : kRules)
        s = std::regex_replace(s, re, with);
    return std::regex_replace(s, std::regex("[ \t\n\r]+"), " ");
}

std::string read_line(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    return line;
}

// A udev database file: its properties (E:) and tags (G:, Q:).
struct UdevEntry {
    std::map<std::string, std::string> props;
    std::vector<std::string> tags;
};

UdevEntry read_udev(const fs::path& p) {
    UdevEntry e;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("E:")) {
            const auto eq = line.find('=');
            if (eq != std::string::npos)
                e.props[line.substr(2, eq - 2)] = line.substr(eq + 1);
        } else if (line.starts_with("G:") || line.starts_with("Q:")) {
            e.tags.push_back(line.substr(2));
        }
    }
    return e;
}

std::string prop(const UdevEntry& e, const char* key) {
    const auto it = e.props.find(key);
    return it == e.props.end() ? "" : it->second;
}

} // namespace

Env card_env(const Card& card) {
    Env env;
    if (card.driver == "nvidia") {
        env = {{"__GLX_VENDOR_LIBRARY_NAME", "nvidia"},
               {"__NV_PRIME_RENDER_OFFLOAD", "1"},
               {"__VK_LAYER_NV_optimus", "NVIDIA_only"}};
    } else if (!card.path_tag.empty()) {
        env = {{"DRI_PRIME", card.path_tag}};
    }
    if (const char* vk = vk_driver_match(card.driver))
        env.emplace_back("VK_LOADER_DRIVERS_SELECT", vk);
    return env;
}

std::string card_name(const Card& card) {
    if (card.vendor.empty() && card.model.empty())
        return "Unknown Graphics Controller";
    if (card.vendor.empty())
        return card.model;
    if (card.model.empty())
        return card.vendor;
    return cleanup(card.vendor + " " + card.model);
}

std::string card_label(const Card& card) {
    const std::string& m = card.model;
    if (const auto open = m.find('['), close = m.rfind(']');
        open != std::string::npos && close != std::string::npos && close > open + 1)
        return m.substr(open + 1, close - open - 1);
    if (!m.empty())
        return m;
    return card_name(card);
}

std::vector<Gpu> from_cards(const std::vector<Card>& cards) {
    std::vector<Gpu> out;
    for (const Card& c : cards) {
        Env env = card_env(c);
        if (env.empty())
            continue;
        out.push_back({card_name(c), card_label(c), std::move(env), c.boot_vga, c.discrete});
    }
    if (out.size() == 1)
        out.front().is_default = true;
    return out;
}

Env pairs(const std::vector<std::string>& flat) {
    Env out;
    for (size_t i = 0; i + 1 < flat.size(); i += 2)
        out.emplace_back(flat[i], flat[i + 1]);
    return out;
}

const Gpu* other(const std::vector<Gpu>& gpus) {
    if (gpus.size() < 2)
        return nullptr;
    for (const Gpu& g : gpus)
        if (!g.is_default && g.discrete)
            return &g;
    for (const Gpu& g : gpus)
        if (!g.is_default)
            return &g;
    return nullptr;
}

const Gpu* preferred(const std::vector<Gpu>& gpus) {
    if (gpus.size() < 2 || std::ranges::any_of(gpus, [](const Gpu& g) { return g.is_default && g.discrete; }))
        return nullptr;
    return other(gpus);
}

bool probe_discrete(const Card& card, const std::string& render_node) {
    if (card.driver == "nvidia" || card.driver == "nouveau")
        return true;
    if (card.driver == "i915")
        return !card.slot.empty() && card.slot != "0000:00:02.0";
    if (card.driver != "amdgpu" && card.driver != "xe")
        return false;
    const int fd = open(render_node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    bool discrete = false;
    if (card.driver == "amdgpu") {
        // AMDGPU_IDS_FLAGS_FUSION is set for all APUs.
        drm_amdgpu_info_device info{};
        drm_amdgpu_info request{};
        request.return_pointer = uintptr_t(&info);
        request.return_size = sizeof(info);
        request.query = AMDGPU_INFO_DEV_INFO;
        discrete = ioctl(fd, DRM_IOCTL_AMDGPU_INFO, &request) == 0 && !(info.ids_flags & AMDGPU_IDS_FLAGS_FUSION);
    } else {
        // Its own VRAM.
        drm_xe_device_query query{};
        query.query = DRM_XE_DEVICE_QUERY_CONFIG;
        if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) == 0 && query.size > 0) {
            std::vector<unsigned char> buf(query.size);
            query.data = uintptr_t(buf.data());
            if (ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) == 0) {
                const auto* config = reinterpret_cast<const drm_xe_query_config*>(buf.data());
                discrete = config->info[DRM_XE_QUERY_CONFIG_FLAGS] & DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM;
            }
        }
    }
    close(fd);
    return discrete;
}

std::vector<Card> scan(const std::string& sys, const std::string& udev_data, const std::string& dev_dir) {
    std::vector<std::pair<int, Card>> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys, ec)) {
        const std::string node = entry.path().filename().string();
        if (!node.starts_with("renderD"))
            continue;
        const fs::path dev = entry.path() / "device";
        Card c;
        c.driver = fs::read_symlink(dev / "driver", ec).filename().string();
        c.boot_vga = read_line(dev / "boot_vga") == "1";
        const UdevEntry self = read_udev(fs::path(udev_data) / ("c" + read_line(entry.path() / "dev")));
        c.path_tag = prop(self, "ID_PATH_TAG");
        const std::string slot = fs::weakly_canonical(dev, ec).filename().string();
        c.slot = slot;
        c.discrete = std::ranges::find(self.tags, "switcheroo-discrete-gpu") != self.tags.end() ||
                     probe_discrete(c, (fs::path(dev_dir) / node).string());
        const UdevEntry parent = read_udev(fs::path(udev_data) / ("+pci:" + slot));
        c.vendor = prop(parent, "SWITCHEROO_CONTROL_VENDOR_NAME");
        if (c.vendor.empty())
            c.vendor = prop(parent, "ID_VENDOR_FROM_DATABASE");
        c.model = prop(parent, "SWITCHEROO_CONTROL_PRODUCT_NAME");
        if (c.model.empty())
            c.model = prop(parent, "ID_MODEL_FROM_DATABASE");
        int n = 0;
        try {
            n = std::stoi(node.substr(7));
        } catch (...) {
        }
        found.emplace_back(n, std::move(c));
    }
    std::ranges::sort(found, {}, &std::pair<int, Card>::first);
    std::vector<Card> cards;
    for (auto& [n, c] : found)
        cards.push_back(std::move(c));
    return cards;
}

} // namespace atrium::gpus
