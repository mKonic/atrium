#include "registry.hpp"

#include <gtest/gtest.h>

using namespace atrium;

TEST(Registry, KeepsSettings) {
    Registry r(":memory:");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.fresh());
    r.set_setting("appearance.corner_radius", 14);
    r.set_setting("appearance.blurred_panels", json::array({"atrium-*"}));
    r.set_setting("appearance.corner_radius", 16);
    auto s = r.settings();
    EXPECT_EQ(s.at("appearance.corner_radius"), 16);
    EXPECT_EQ(s.at("appearance.blurred_panels"), json::array({"atrium-*"}));
    r.erase_setting("appearance.corner_radius");
    EXPECT_FALSE(r.settings().contains("appearance.corner_radius"));
}

TEST(Registry, AppsAreRecords) {
    Registry r(":memory:");
    r.put_app({.app_id = "vesktop", .secret = "communication", .launch = "vesktop"});
    r.put_app({.app_id = "org.kde.dolphin", .dock = 0});
    ASSERT_TRUE(r.app("VESKTOP"));  // ids match without regard to case
    EXPECT_EQ(r.app("vesktop")->launch, "vesktop");

    AppRecord a = *r.app("vesktop");
    a.placement = Placement{"DP-1", 10, 20, 800, 600, false, 0};
    r.put_app(a);
    EXPECT_EQ(r.app("vesktop")->placement->width, 800);

    r.set_dock({"vesktop", "org.kde.dolphin"});
    EXPECT_EQ(r.app("vesktop")->dock, 0);
    EXPECT_EQ(r.app("org.kde.dolphin")->dock, 1);
    r.set_dock({"vesktop"});
    EXPECT_FALSE(r.app("org.kde.dolphin"));  // only pinned: gone once unpinned
    EXPECT_TRUE(r.app("vesktop"));           // still has its space

    r.set_dock({"", "vesktop"});  // blanks are no app
    EXPECT_EQ(r.app("vesktop")->dock, 0);
    EXPECT_FALSE(r.app(""));

    r.put_app({.app_id = "vesktop"});  // nothing left: removed
    EXPECT_FALSE(r.app("vesktop"));
}

TEST(Registry, RulesAndShortcuts) {
    Registry r(":memory:");
    const int64_t id = r.add_rule({.app_pattern = "^firefox$", .title_pattern = "Picture-in-Picture", .space = 2});
    ASSERT_GT(id, 0);
    RuleRecord rule = r.rules().front();
    rule.space = 3;
    EXPECT_TRUE(r.update_rule(rule));
    EXPECT_EQ(r.rules().front().space, 3);
    EXPECT_TRUE(r.remove_rule(id));
    EXPECT_TRUE(r.rules().empty());

    r.replace_shortcuts({{0, "Mod+Q", "close", "", false}, {0, "XF86AudioMute", "shell", "volume-mute", true}});
    auto keys = r.shortcuts();
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[1].arg, "volume-mute");
    EXPECT_TRUE(keys[1].locked);
    keys[0].keys = "Mod+W";
    EXPECT_TRUE(r.update_shortcut(keys[0]));
    EXPECT_EQ(r.shortcuts()[0].keys, "Mod+W");
}

TEST(Registry, ImportsTheOldStores) {
    std::vector<RuleRecord> leftover;
    const auto apps = apps_from_legacy(
        json::parse(R"([{"app_id": "discord|^Vesktop$|whatsapp", "secret": "communication"},
                        {"app_id": "^firefox$", "title": "Picture-in-Picture", "space": 2},
                        {"app_id": "steam_app_.*", "fullscreen": true}])"),
        json::parse(R"(["org.kde.dolphin", "discord"])"),
        json::parse(R"({"foot": {"output": "DP-1", "x": 1, "y": 2, "width": 700, "height": 500}})"), &leftover);
    auto find = [&](const std::string& id) {
        for (const auto& a : apps)
            if (a.app_id == id)
                return a;
        return AppRecord{};
    };
    EXPECT_EQ(find("Vesktop").secret, "communication");
    EXPECT_EQ(find("discord").secret, "communication");
    EXPECT_EQ(find("discord").dock, 1);
    EXPECT_EQ(find("org.kde.dolphin").dock, 0);
    EXPECT_EQ(find("foot").placement->width, 700);
    ASSERT_EQ(leftover.size(), 2u);  // a title and a real pattern stay rules
    EXPECT_EQ(leftover[0].title_pattern, "Picture-in-Picture");
}
