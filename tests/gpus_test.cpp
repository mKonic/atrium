#include "../shell/plugin/Atrium/Shell/gpus_core.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace atrium::gpus;
namespace fs = std::filesystem;

namespace {

const Card kNvidia{.driver = "nvidia", .path_tag = "pci-0000_01_00_0", .vendor = "NVIDIA Corporation",
                  .model = "GB206 [GeForce RTX 5060 Ti]", .slot = "0000:01:00.0", .boot_vga = true, .discrete = true};
const Card kAmd{.driver = "amdgpu", .path_tag = "pci-0000_10_00_0", .vendor = "Advanced Micro Devices, Inc. [AMD/ATI]",
                .model = "Raphael [Radeon 610M]", .slot = "0000:10:00.0"};

TEST(Gpus, EachDriverGetsSwitcheroosVariables) {
    EXPECT_EQ(card_env(kNvidia), (Env{{"__GLX_VENDOR_LIBRARY_NAME", "nvidia"},
                                      {"__NV_PRIME_RENDER_OFFLOAD", "1"},
                                      {"__VK_LAYER_NV_optimus", "NVIDIA_only"},
                                      {"VK_LOADER_DRIVERS_SELECT", "*nvidia*"}}));
    EXPECT_EQ(card_env(kAmd), (Env{{"DRI_PRIME", "pci-0000_10_00_0"}, {"VK_LOADER_DRIVERS_SELECT", "*radeon*"}}));
    // Nothing to send it there with: not a GPU apps can be started on.
    EXPECT_TRUE(card_env(Card{.driver = "simpledrm"}).empty());
}

TEST(Gpus, NamesAreSwitcheroosTidied) {
    EXPECT_EQ(card_name(kNvidia), "NVIDIA Corporation GB206 [GeForce RTX™ 5060 Ti]");
    EXPECT_EQ(card_name(kAmd), "Advanced Micro Devices, Inc. [AMD/ATI] Raphael [Radeon™ 610M]");
    EXPECT_EQ(card_name(Card{.driver = "i915", .path_tag = "pci-x"}), "Unknown Graphics Controller");
    EXPECT_EQ(card_label(kNvidia), "GeForce RTX 5060 Ti");
    EXPECT_EQ(card_label(kAmd), "Radeon 610M");
}

TEST(Gpus, TheBootGpuIsTheDefaultAndAppsGoToTheOther) {
    const auto gpus = from_cards({kNvidia, kAmd});
    ASSERT_EQ(gpus.size(), 2u);
    EXPECT_TRUE(gpus[0].is_default);
    EXPECT_FALSE(gpus[1].is_default);
    ASSERT_TRUE(other(gpus));
    EXPECT_EQ(other(gpus)->label, "Radeon 610M");
    // One GPU: it's the default, whatever boot_vga says, and there's no other.
    Card lone = kAmd;
    const auto one = from_cards({lone});
    EXPECT_TRUE(one[0].is_default);
    EXPECT_EQ(other(one), nullptr);
}

TEST(Gpus, ADiscreteOneIsPreferred) {
    const Card igpu{.driver = "i915", .path_tag = "pci-0000_00_02_0", .vendor = "Intel", .model = "UHD [UHD Graphics]",
                    .slot = "0000:00:02.0", .boot_vga = true};
    Card b = kNvidia;
    b.boot_vga = false;
    const auto gpus = from_cards({igpu, kAmd, b});
    EXPECT_EQ(other(gpus)->label, "GeForce RTX 5060 Ti");
    // A laptop: a game that prefers the non-default GPU gets the discrete one.
    EXPECT_EQ(preferred(gpus)->label, "GeForce RTX 5060 Ti");
}

TEST(Gpus, OnADesktopTheGraphicsCardIsAlreadyWhereGamesGo) {
    // The boot GPU is the discrete card (a desktop with an APU beside it):
    // PrefersNonDefaultGPU keeps it there; "Launch on …" still offers the APU.
    const auto gpus = from_cards({kNvidia, kAmd});
    EXPECT_EQ(preferred(gpus), nullptr);
    ASSERT_TRUE(other(gpus));
    EXPECT_EQ(other(gpus)->label, "Radeon 610M");
}

TEST(Gpus, SwitcheroosDiscreteRulesByDriver) {
    EXPECT_TRUE(probe_discrete(kNvidia, "/nonexistent"));
    EXPECT_TRUE(probe_discrete(Card{.driver = "nouveau"}, "/nonexistent"));
    EXPECT_FALSE(probe_discrete(Card{.driver = "i915", .slot = "0000:00:02.0"}, "/nonexistent"));
    EXPECT_TRUE(probe_discrete(Card{.driver = "i915", .slot = "0000:03:00.0"}, "/nonexistent"));
    // amdgpu and xe ask the device; one that can't be opened isn't counted.
    EXPECT_FALSE(probe_discrete(kAmd, "/nonexistent"));
}

TEST(Gpus, SwitcherooEnvironmentPairsUp) {
    EXPECT_EQ(pairs({"A", "1", "B", "2", "dangling"}), (Env{{"A", "1"}, {"B", "2"}}));
}

TEST(Gpus, ScanReadsSysfsAndUdev) {
    const fs::path root = fs::path(::testing::TempDir()) / "atrium-gpus";
    fs::remove_all(root);
    const fs::path sys = root / "sys", udev = root / "udev", pci = root / "pci";
    auto write = [](const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    };
    // Two render nodes, listed out of order; each device a PCI function.
    for (auto [node, slot, driver, boot, minor] :
         {std::tuple{"renderD129", "0000:10:00.0", "amdgpu", "0", "129"},
          std::tuple{"renderD128", "0000:01:00.0", "nvidia", "1", "128"}}) {
        const fs::path dev = pci / slot;
        write(dev / "boot_vga", std::string(boot) + "\n");
        fs::create_directories(root / "drivers" / driver);
        fs::create_directory_symlink(root / "drivers" / driver, dev / "driver");
        fs::create_directories(sys / node);
        fs::create_directory_symlink(dev, sys / node / "device");
        write(sys / node / "dev", std::string("226:") + minor + "\n");
    }
    write(sys / "card0" / "dev", "226:0\n");  // not a render node
    write(udev / "c226:128", "E:ID_PATH_TAG=pci-0000_01_00_0\n");
    write(udev / "c226:129", "E:ID_PATH_TAG=pci-0000_10_00_0\nG:switcheroo-discrete-gpu\n");
    write(udev / "+pci:0000:01:00.0", "E:ID_VENDOR_FROM_DATABASE=NVIDIA Corporation\n"
                                      "E:ID_MODEL_FROM_DATABASE=GB206 [GeForce RTX 5060 Ti]\n");
    write(udev / "+pci:0000:10:00.0", "E:ID_VENDOR_FROM_DATABASE=AMD\nE:ID_MODEL_FROM_DATABASE=Raphael [Radeon 610M]\n"
                                      "E:SWITCHEROO_CONTROL_PRODUCT_NAME=Radeon 610M iGPU\n");

    const auto cards = scan(sys.string(), udev.string());
    ASSERT_EQ(cards.size(), 2u);
    EXPECT_EQ(cards[0].driver, "nvidia");
    EXPECT_TRUE(cards[0].boot_vga);
    EXPECT_EQ(cards[0].path_tag, "pci-0000_01_00_0");
    EXPECT_EQ(cards[0].model, "GB206 [GeForce RTX 5060 Ti]");
    EXPECT_EQ(cards[1].driver, "amdgpu");
    EXPECT_FALSE(cards[1].boot_vga);
    EXPECT_TRUE(cards[1].discrete);
    EXPECT_EQ(cards[1].model, "Radeon 610M iGPU");  // switcheroo's own name wins
    fs::remove_all(root);
}

} // namespace
