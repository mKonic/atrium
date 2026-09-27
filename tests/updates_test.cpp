#include "updates_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::updates;

TEST(Updates, PackageIds) {
    const PackageId p = parse_package_id("firefox;131.0-1;x86_64;extra");
    EXPECT_EQ(p.name, "firefox");
    EXPECT_EQ(p.version, "131.0-1");
    EXPECT_EQ(p.arch, "x86_64");
    EXPECT_EQ(p.repo, "extra");
    EXPECT_EQ(parse_package_id("tzdata;2026a-1;any;").repo, "");
    EXPECT_EQ(parse_package_id("nonsense").name, "");
    EXPECT_EQ(parse_package_id(";1;x86_64;core").name, "");
}

TEST(Updates, Words) {
    EXPECT_EQ(doing(pk::kInfoDownloading), "Downloading");
    EXPECT_EQ(doing(pk::kInfoUpdating), "Installing");
    EXPECT_EQ(doing(pk::kInfoSecurity), "");
    EXPECT_EQ(summary(0, 0), "No updates");
    EXPECT_EQ(summary(1, 0), "1 update");
    EXPECT_EQ(summary(5, 2), "5 updates, 2 for security");
}

TEST(Updates, Restart) {
    EXPECT_TRUE(needs_restart("linux"));
    EXPECT_TRUE(needs_restart("linux-cachyos"));
    EXPECT_TRUE(needs_restart("linux-lts"));
    EXPECT_TRUE(needs_restart("nvidia-utils"));
    EXPECT_TRUE(needs_restart("systemd"));
    EXPECT_FALSE(needs_restart("linux-api-headers"));
    EXPECT_FALSE(needs_restart("firefox"));
    EXPECT_FALSE(needs_restart("systemd-libs-extra"));
}


TEST(Updates, ParsesUpgradeLists) {
    const auto u = atrium::updates::parse_upgrades(
        "firefox 131.0-1 -> 132.0-1\n\x1b[1mparu-bin\x1b[0m 2.0.3-1 -> 2.0.4-1\nwarning: whatever\nlinux 6.1-1 -> 6.2-1 [ignored]\n");
    ASSERT_EQ(u.size(), 2u);  // the ignored one isn't an update
    EXPECT_EQ(u[0].name, "firefox");
    EXPECT_EQ(u[1].name, "paru-bin");
    EXPECT_EQ(u[1].to, "2.0.4-1");
}

TEST(Updates, PacmanSteps) {
    atrium::updates::Step s;
    ASSERT_TRUE(atrium::updates::parse_pacman_step("(3/10) upgrading firefox          [####]  100%", s));
    EXPECT_EQ(s.done, 3);
    EXPECT_EQ(s.total, 10);
    EXPECT_EQ(s.what, "Upgrading firefox");
    EXPECT_FALSE(atrium::updates::parse_pacman_step("(1/2) checking keys in keyring", s));
    EXPECT_FALSE(atrium::updates::parse_pacman_step(":: Synchronizing package databases...", s));
}

TEST(Updates, ReleaseOrder) {
    using atrium::updates::compare_tags;
    using atrium::updates::release_newer;
    EXPECT_LT(compare_tags("v1.2.0", "v1.10.0"), 0);
    EXPECT_LT(compare_tags("v1.4.0-rc.1", "v1.4.0"), 0);
    EXPECT_LT(compare_tags("v1.4.0-rc.2", "v1.4.0-rc.10"), 0);
    EXPECT_EQ(compare_tags("v1.0.0+build.5", "v1.0.0"), 0);
    EXPECT_TRUE(release_newer("v0.2.0", "v0.1.0"));
    EXPECT_TRUE(release_newer("v0.2.0", "v0.1.0-14-gabc1234-dirty"));
    EXPECT_FALSE(release_newer("v0.1.0", "v0.1.0-14-gabc1234"));  // a build past the tag has it
    EXPECT_FALSE(release_newer("v0.1.0", "v0.1.0"));
    EXPECT_TRUE(release_newer("v0.1.0", "abc1234"));               // before any tag
    EXPECT_FALSE(release_newer("nightly", "v0.1.0"));
}

TEST(Updates, PkgverOfABuild) {
    using atrium::updates::pkgver;
    EXPECT_EQ(pkgver("v0.1.0", 10240), "0.1.0");
    EXPECT_EQ(pkgver("v0.1.0-5-gabc1234", 10245), "0.1.0.r5.gabc1234");
    EXPECT_EQ(pkgver("v0.1.0-5-gabc1234-dirty", 10245), "0.1.0.r5.gabc1234");
    EXPECT_EQ(pkgver("v1.4.0-rc.1", 10300), "1.4.0rc.1");
    EXPECT_EQ(pkgver("5076b9b", 10233), "0.r233.5076b9b");
}
