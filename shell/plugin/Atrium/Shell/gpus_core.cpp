#include "gpus_core.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace atrium::gpus {

Env offload_env(const Card& card) {
    // NVIDIA's own driver: its PRIME offload switches (GLX, EGL and Vulkan).
    if (card.driver == "nvidia")
        return {{"__NV_PRIME_RENDER_OFFLOAD", "1"},
                {"__GLX_VENDOR_LIBRARY_NAME", "nvidia"},
                {"__VK_LAYER_NV_optimus", "NVIDIA_only"}};
    // Mesa: the card by its PCI slot, colons and dots made underscores.
    std::string tag = "pci-" + card.slot;
    std::ranges::replace(tag, ':', '_');
    std::ranges::replace(tag, '.', '_');
    return {{"DRI_PRIME", tag}};
}

namespace {

uint16_t hex16(std::string_view s) {
    uint16_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v, 16);
    return v;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

} // namespace

std::string pci_name(std::string_view ids, uint16_t vendor, uint16_t device) {
    std::string vendor_name;
    bool in_vendor = false;
    while (!ids.empty()) {
        const size_t nl = ids.find('\n');
        std::string_view line = ids.substr(0, nl);
        ids = nl == std::string_view::npos ? std::string_view{} : ids.substr(nl + 1);
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] != '\t') {
            if (in_vendor)
                break;  // past its devices
            if (line.size() > 6 && hex16(line.substr(0, 4)) == vendor && line[4] == ' ') {
                in_vendor = true;
                vendor_name = std::string(trim(line.substr(4)));
            }
            continue;
        }
        if (!in_vendor || line.size() < 6 || line[1] == '\t')
            continue;  // subsystems
        if (hex16(line.substr(1, 4)) != device)
            continue;
        std::string_view model = trim(line.substr(5));
        // The marketing name is in brackets when there is one.
        if (const size_t open = model.find('['), close = model.rfind(']');
            open != std::string_view::npos && close != std::string_view::npos && close > open + 1)
            model = model.substr(open + 1, close - open - 1);
        return std::string(model);
    }
    return vendor_name;
}

std::vector<Gpu> from_cards(std::vector<Card> cards, std::string_view pci_ids) {
    std::stable_partition(cards.begin(), cards.end(), [](const Card& c) { return c.boot_vga; });
    std::vector<Gpu> out;
    for (const Card& c : cards) {
        Gpu g;
        g.name = pci_name(pci_ids, c.vendor, c.device);
        if (g.name.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "GPU %04x:%04x", c.vendor, c.device);
            g.name = buf;
        }
        g.env = offload_env(c);
        g.is_default = out.empty();
        out.push_back(std::move(g));
    }
    return out;
}

Env pairs(const std::vector<std::string>& flat) {
    Env out;
    for (size_t i = 0; i + 1 < flat.size(); i += 2)
        out.emplace_back(flat[i], flat[i + 1]);
    return out;
}

const Gpu* other(const std::vector<Gpu>& gpus) {
    for (const Gpu& g : gpus)
        if (!g.is_default)
            return &g;
    return nullptr;
}

} // namespace atrium::gpus
