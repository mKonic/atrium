#include "gpus_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::gpus;

namespace {

const char* kIds = R"(# pci.ids excerpt
1002  Advanced Micro Devices, Inc. [AMD/ATI]
	13c0  Granite Ridge [Radeon Graphics]
	164e  Raphael
		1002 164e  Raphael (subsystem)
10de  NVIDIA Corporation
	2684  AD102 [GeForce RTX 4090]
	2c05  GB203 [GeForce RTX 5070 Ti]
8086  Intel Corporation
)";

} // namespace

TEST(Gpus, NamesFromPciIds) {
    EXPECT_EQ(pci_name(kIds, 0x10de, 0x2c05), "GeForce RTX 5070 Ti");
    EXPECT_EQ(pci_name(kIds, 0x1002, 0x164e), "Raphael");  // no bracket, and not the subsystem line
    EXPECT_EQ(pci_name(kIds, 0x10de, 0x9999), "NVIDIA Corporation");
    EXPECT_EQ(pci_name(kIds, 0x1234, 0x0001), "");
}

TEST(Gpus, OffloadEnvironmentPerDriver) {
    const Env nv = offload_env({"nvidia", "0000:01:00.0", 0x10de, 0x2c05, false});
    EXPECT_EQ(nv.size(), 3u);
    EXPECT_EQ(nv[0], (std::pair<std::string, std::string>{"__NV_PRIME_RENDER_OFFLOAD", "1"}));
    const Env amd = offload_env({"amdgpu", "0000:7a:00.0", 0x1002, 0x164e, false});
    EXPECT_EQ(amd, (Env{{"DRI_PRIME", "pci-0000_7a_00_0"}}));
}

TEST(Gpus, TheBootGpuIsTheDefault) {
    const auto g = from_cards({{"amdgpu", "0000:7a:00.0", 0x1002, 0x164e, false},
                               {"nvidia", "0000:01:00.0", 0x10de, 0x2c05, true}},
                              kIds);
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[0].name, "GeForce RTX 5070 Ti");
    EXPECT_TRUE(g[0].is_default);
    ASSERT_NE(other(g), nullptr);
    EXPECT_EQ(other(g)->name, "Raphael");
    EXPECT_EQ(other(g)->env, (Env{{"DRI_PRIME", "pci-0000_7a_00_0"}}));
}

TEST(Gpus, OneGpuHasNoOther) {
    EXPECT_EQ(other(from_cards({{"i915", "0000:00:02.0", 0x8086, 0x46a6, false}}, kIds)), nullptr);
    EXPECT_EQ(from_cards({{"i915", "0000:00:02.0", 0x8086, 0x46a6, false}}, kIds)[0].name, "Intel Corporation");
}

TEST(Gpus, SwitcherooEnvironmentPairs) {
    EXPECT_EQ(pairs({"DRI_PRIME", "1", "FOO"}), (Env{{"DRI_PRIME", "1"}}));
}
