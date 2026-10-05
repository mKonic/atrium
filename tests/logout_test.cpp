#include "../src/logout.hpp"

#include <gtest/gtest.h>

using namespace atrium;

namespace {

TEST(Logout, ReopensEachAppOnceButNotTheShells) {
    EXPECT_EQ(apps_to_reopen({"firefox", "org.kde.dolphin", "firefox", "", "atrium-shell", "atrium-capture-region",
                              "foot"}),
              (std::vector<std::string>{"firefox", "org.kde.dolphin", "foot"}));
    EXPECT_TRUE(apps_to_reopen({}).empty());
}

TEST(Logout, NamesAppsReadably) {
    EXPECT_EQ(app_name("org.kde.dolphin"), "Dolphin");
    EXPECT_EQ(app_name("firefox"), "Firefox");
    EXPECT_EQ(app_name("com.example."), "Com.example.");
    EXPECT_EQ(app_name(""), "");
}

TEST(Logout, ReadsWhatComesAfter) {
    EXPECT_EQ(Logout::parse(""), Logout::Then::LogOut);
    EXPECT_EQ(Logout::parse("restart"), Logout::Then::Restart);
    EXPECT_EQ(Logout::parse("shutdown"), Logout::Then::ShutDown);
    EXPECT_EQ(Logout::parse("shut-down"), Logout::Then::ShutDown);
    EXPECT_EQ(Logout::parse("sideways"), Logout::Then::LogOut);
}

} // namespace
