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
