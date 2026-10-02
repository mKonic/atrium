#pragma once
// Which GPUs an app can be started on, as switcheroo-control decides it (and
// atrium does itself where switcheroo isn't installed): the firmware's boot
// GPU is the default, the others are offloaded to through environment
// variables (PRIME render offload). Kept free of Qt to be testable.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium::gpus {

using Env = std::vector<std::pair<std::string, std::string>>;

struct Gpu {
    std::string name;
    Env env;  // what an app started on it gets
    bool is_default = false;
};

// One DRM card as sysfs tells it.
struct Card {
    std::string driver;  // "nvidia", "amdgpu", "i915"
    std::string slot;    // PCI address, "0000:01:00.0"
    uint16_t vendor = 0, device = 0;
    bool boot_vga = false;
};

// The variables that send an app's rendering to `card`.
Env offload_env(const Card& card);
// A card's model from pci.ids ("AD102 [GeForce RTX 4090]" → "GeForce RTX
// 4090"), its vendor's name if the model isn't listed, "" if neither is.
std::string pci_name(std::string_view pci_ids, uint16_t vendor, uint16_t device);
// The GPUs, the boot one first and marked default (the first if none is).
std::vector<Gpu> from_cards(std::vector<Card> cards, std::string_view pci_ids);
// switcheroo-control's Environment: KEY, VALUE, KEY, VALUE...
Env pairs(const std::vector<std::string>& flat);
// Where an app that prefers the non-default GPU goes: null with one GPU.
const Gpu* other(const std::vector<Gpu>& gpus);

} // namespace atrium::gpus
