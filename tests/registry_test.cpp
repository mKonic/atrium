#include "registry.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdio>
#include <filesystem>
#include <optional>

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

TEST(Registry, RemembersDisplays) {
    Registry r(":memory:");
    EXPECT_FALSE(r.display("Dell U2720Q 123"));
    r.put_display({.id = "Dell U2720Q 123", .width = 3840, .height = 2160, .refresh = 60000, .scale = 1.5, .x = 1920, .y = 0});
    auto d = r.display("Dell U2720Q 123");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->width, 3840);
    EXPECT_DOUBLE_EQ(d->scale, 1.5);
    EXPECT_EQ(d->x, 1920);
    d->enabled = false;
    d->x.reset();
    r.put_display(*d);
    EXPECT_FALSE(r.display("Dell U2720Q 123")->enabled);
    EXPECT_FALSE(r.display("Dell U2720Q 123")->x);
}

// A registry from before directional keys: its untouched arrow defaults
// become caelestia's, what the user changed stays, and the new keys arrive.
TEST(Registry, MigratesOldArrowDefaults) {
    const std::string path = ::testing::TempDir() + "atrium-migrate.db";
    std::remove(path.c_str());
    {
        Registry r(path);
        ASSERT_TRUE(r.ok());
        r.replace_shortcuts({{0, "Mod+Left", "snap-left", "", false},
                             {0, "Mod+Right", "terminal", "", false},  // changed by the user
                             {0, "Mod+Up", "maximize", "", false},
                             {0, "Mod+Down", "restore", "", false}});
    }
    {
        // Back to how version 2 left it.
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        sqlite3_exec(db, "PRAGMA user_version=2; DELETE FROM shortcuts WHERE action = 'toggle-tiling'", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    Registry r(path);
    auto find = [&](const std::string& keys) -> std::optional<ShortcutRecord> {
        for (const ShortcutRecord& s : r.shortcuts())
            if (s.keys == keys)
                return s;
        return std::nullopt;
    };
    ASSERT_TRUE(find("Mod+Left"));
    EXPECT_EQ(find("Mod+Left")->action, "focus-direction");
    EXPECT_EQ(find("Mod+Left")->arg, "left");
    EXPECT_EQ(find("Mod+Right")->action, "terminal");
    EXPECT_EQ(find("Mod+Alt+F")->action, "maximize");
    EXPECT_EQ(find("Mod+Up")->action, "focus-direction");
    EXPECT_EQ(find("Mod+Down")->arg, "down");
    EXPECT_EQ(find("Mod+Shift+Left")->action, "move-direction");
    EXPECT_EQ(find("Mod+0")->arg, "10");
    EXPECT_EQ(find("Mod+backslash")->action, "toggle-tiling");
    EXPECT_EQ(find("Mod+Ctrl+Down")->action, "app-expose");
    EXPECT_EQ(find("Mod+period")->arg, "emoji");
    std::remove(path.c_str());
}

TEST(Registry, DevicesKeepOnlyWhatTheySet) {
    Registry r(":memory:");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.device("Mouse"));
    r.put_device({"Mouse", 0.4, std::nullopt, true, std::nullopt});
    auto d = r.device("Mouse");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->speed, 0.4);
    EXPECT_FALSE(d->acceleration);
    EXPECT_EQ(d->natural_scroll, true);
    EXPECT_FALSE(d->left_handed);
    r.put_device({"Touchpad", std::nullopt, "flat", std::nullopt, std::nullopt});
    EXPECT_EQ(r.devices().size(), 2u);
    // Nothing of its own left: it follows the shared settings again.
    r.put_device({"Mouse"});
    EXPECT_FALSE(r.device("Mouse"));
    EXPECT_EQ(r.devices().size(), 1u);
}

TEST(Registry, SessionsKeepTheirWindows) {
    Registry r(":memory:");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.has_session("abc"));
    r.touch_session("abc", "org.example.Editor");
    EXPECT_TRUE(r.has_session("abc"));
    const SessionWindow main{"main", Placement{"DP-1", 40, 60, 800, 600, false, 0}, false};
    r.put_session_window("abc", main);
    EXPECT_EQ(r.session_window("abc", "main"), main);
    EXPECT_FALSE(r.session_window("abc", "other"));
    EXPECT_FALSE(r.session_window("xyz", "main"));
    r.rename_session_window("abc", "main", "document-1");
    EXPECT_FALSE(r.session_window("abc", "main"));
    EXPECT_TRUE(r.session_window("abc", "document-1"));
    // Unused for long enough, a session goes with its windows.
    r.drop_sessions_before(0);
    EXPECT_TRUE(r.has_session("abc"));
    r.drop_sessions_before(INT64_MAX);
    EXPECT_FALSE(r.has_session("abc"));
    EXPECT_FALSE(r.session_window("abc", "document-1"));
}

TEST(Registry, BacksUpAnOlderFileBeforeMigrating) {
    const std::string dir = testing::TempDir() + "atrium-registry-backup";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string file = dir + "/registry.db";
    {
        // A registry from an older atrium: schema 1, one setting.
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(file.c_str(), &db), SQLITE_OK);
        sqlite3_exec(db, "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                         "INSERT INTO settings VALUES ('appearance.corner_radius', '14');"
                         "PRAGMA user_version=1;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        Registry r(file);
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r.settings().at("appearance.corner_radius"), 14);
    }
    EXPECT_TRUE(std::filesystem::exists(file + ".v1.bak"));
    // A fresh one has nothing to keep.
    Registry fresh(dir + "/fresh.db");
    EXPECT_FALSE(std::filesystem::exists(dir + "/fresh.db.v0.bak"));
    std::filesystem::remove_all(dir);
}
