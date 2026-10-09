#pragma once
// Which GPUs an app can be started on, by switcheroo-control's rules
// (switcheroo-control.c), read from sysfs and udev's database where it isn't
// running: one GPU per render node; the firmware's boot GPU (boot_vga) is
// the default; an app goes to another through the environment that card's
// driver understands (PRIME render offload). Kept free of Qt to be testable.
#include <string>
#include <utility>
#include <vector>

namespace atrium::gpus {

using Env = std::vector<std::pair<std::string, std::string>>;

struct Gpu {
    std::string name;   // switcheroo's: vendor and model, tidied
    std::string label;  // short, for menus: "GeForce RTX 5060 Ti"
    Env env;            // what an app started on it gets
    bool is_default = false;
    bool discrete = false;
};

// A render node's card, as sysfs and udev describe it.
struct Card {
    std::string driver;    // the PCI device's kernel driver: "nvidia", "amdgpu"
    std::string path_tag;  // the render node's ID_PATH_TAG: "pci-0000_10_00_0"
    std::string vendor;    // ID_VENDOR_FROM_DATABASE (or switcheroo's override)
    std::string model;     // ID_MODEL_FROM_DATABASE (or switcheroo's override)
    std::string slot;      // its PCI address: "0000:10:00.0"
    bool boot_vga = false;
    bool discrete = false;  // switcheroo's udev tag, or its checks (scan)
};

// switcheroo's get_card_env: none when it can't send apps there.
Env card_env(const Card& card);
// switcheroo's get_card_name ("Unknown Graphics Controller" when udev knows
// neither), with info_cleanup's tidying.
std::string card_name(const Card& card);
// The model's marketing name ("Raphael [Radeon 610M]" → "Radeon 610M").
std::string card_label(const Card& card);
// The GPUs in render node order; the only one is the default.
std::vector<Gpu> from_cards(const std::vector<Card>& cards);
// switcheroo-control's Environment: KEY, VALUE, KEY, VALUE...
Env pairs(const std::vector<std::string>& flat);
// "Launch on …": the GPU apps don't start on by default (a discrete one
// first); none with one GPU.
const Gpu* other(const std::vector<Gpu>& gpus);
// Where an app with PrefersNonDefaultGPU goes ("prefers to be run on a more
// powerful discrete GPU"): none when the default is the discrete one (a
// desktop's graphics card), else a discrete one, else the other.
const Gpu* preferred(const std::vector<Gpu>& gpus);
// switcheroo-control's discrete-detection, by driver: nvidia always, i915
// unless at 00:02.0, amdgpu unless an APU, xe with its own VRAM; nouveau
// taken as discrete. `render_node` is opened for the ioctls.
bool probe_discrete(const Card& card, const std::string& render_node);
// The render nodes' cards under `sys` (/sys/class/drm) and `udev_data`
// (/run/udev/data).
std::vector<Card> scan(const std::string& sys = "/sys/class/drm", const std::string& udev_data = "/run/udev/data",
                       const std::string& dev = "/dev/dri");

} // namespace atrium::gpus
