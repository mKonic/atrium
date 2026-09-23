#include "settings.hpp"

#include <gtest/gtest.h>


using namespace atrium;

TEST(Chord, ParsesModifiersAndKey) {
    auto c = parse_chord("Mod+Shift+E");
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->uses_mod);
    EXPECT_EQ(c->mods, uint32_t(WLR_MODIFIER_SHIFT));
    EXPECT_EQ(c->sym, xkb_keysym_t(XKB_KEY_e));
}

TEST(Chord, NamedKeysAndCase) {
    EXPECT_EQ(parse_chord("super+return")->sym, xkb_keysym_t(XKB_KEY_Return));
    EXPECT_EQ(parse_chord("Ctrl+Alt+F3")->mods, uint32_t(WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT));
    EXPECT_EQ(parse_chord("Mod++")->sym, xkb_keysym_t(XKB_KEY_plus));
}

TEST(Chord, RejectsNonsense) {
    EXPECT_FALSE(parse_chord(""));
    EXPECT_FALSE(parse_chord("Mod+"));
    EXPECT_FALSE(parse_chord("Hyper+Q"));
    EXPECT_FALSE(parse_chord("Mod+NotAKey"));
}

TEST(Color, RoundTrips) {
    auto c = parse_color("#ff8000");
    ASSERT_TRUE(c);
    EXPECT_FLOAT_EQ((*c)[0], 1.0f);
    EXPECT_FLOAT_EQ((*c)[3], 1.0f);
    EXPECT_EQ(format_color(*c), "#ff8000ff");
    EXPECT_EQ(format_color(*parse_color("#1a1b26cc")), "#1a1b26cc");
    EXPECT_FALSE(parse_color("ff8000"));
    EXPECT_FALSE(parse_color("#ff80"));
    EXPECT_FALSE(parse_color("#gg0000"));
}

TEST(Settings, DefaultsComeFromTheConfig) {
    Settings s(Config::defaults(false), nullptr);
    EXPECT_EQ(s.get("appearance.corner_radius"), 12);
    EXPECT_EQ(s.get("shortcuts.modifier"), "super");
    EXPECT_EQ(Settings(Config::defaults(true), nullptr).get("shortcuts.modifier"), "alt");
}

TEST(Settings, ValidatesAgainstTheSchema) {
    Settings s(Config::defaults(false), nullptr);
    EXPECT_FALSE(s.set("appearance.corner_radius", 20));
    EXPECT_TRUE(s.set("appearance.corner_radius", 500));     // out of range
    EXPECT_TRUE(s.set("appearance.corner_radius", "big"));   // wrong type
    EXPECT_TRUE(s.set("appearance.shadows", 1));             // bool is not a number
    EXPECT_TRUE(s.set("pointer.acceleration", "wild"));      // not a choice
    EXPECT_TRUE(s.set("appearance.shadow_color", "black"));  // not a color
    EXPECT_TRUE(s.set("no.such.key", 1));
    EXPECT_EQ(s.get("appearance.corner_radius"), 20);
}

TEST(Settings, AcceptsWholeFloatsForIntegers) {
    Settings s(Config::defaults(false), nullptr);
    EXPECT_FALSE(s.set("appearance.corner_radius", 16.0));
    EXPECT_TRUE(s.get("appearance.corner_radius").is_number_integer());
    EXPECT_TRUE(s.set("appearance.corner_radius", 16.5));
}

TEST(Settings, AppliesToTheConfig) {
    Settings s(Config::defaults(false), nullptr);
    ASSERT_FALSE(s.set("appearance.corner_radius", 3));
    ASSERT_FALSE(s.set("pointer.acceleration", "flat"));
    ASSERT_FALSE(s.set("appearance.shadow_color", "#ff0000"));
    ASSERT_FALSE(s.set("shortcuts.modifier", "alt"));
    Config c = Config::defaults(false);
    s.apply(c);
    EXPECT_EQ(c.corner_radius, 3);
    EXPECT_EQ(c.accel_profile, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
    EXPECT_FLOAT_EQ(c.shadow_color[0], 1.0f);
    EXPECT_EQ(c.mod, uint32_t(WLR_MODIFIER_ALT));
}

TEST(Shortcuts, FollowTheModifier) {
    const auto alt = resolve_keybinds(default_keybinds(), WLR_MODIFIER_ALT);
    EXPECT_NE(find_keybind(alt, WLR_MODIFIER_ALT, XKB_KEY_q), nullptr);
    EXPECT_EQ(find_keybind(alt, WLR_MODIFIER_LOGO, XKB_KEY_q), nullptr);
}

TEST(Shortcuts, ClashesAreTheSameKeysHoweverWritten) {
    const std::vector<ShortcutRecord> list{{1, "Mod+Q", "close", ""},
                                           {2, "Super+q", "terminal", ""},
                                           {3, "Shift+Mod+E", "quit", ""},
                                           {4, "Mod+Shift+E", "overview", ""},
                                           {5, "Mod+W", "close", ""}};
    const auto super = shortcut_clashes(list, WLR_MODIFIER_LOGO);
    EXPECT_EQ(super.at(1), std::vector<int64_t>{2});
    EXPECT_EQ(super.at(2), std::vector<int64_t>{1});
    EXPECT_EQ(super.at(3), std::vector<int64_t>{4});
    EXPECT_FALSE(super.contains(5));
    // With Alt as Mod, Mod+Q and Super+Q are different keys.
    EXPECT_FALSE(shortcut_clashes(list, WLR_MODIFIER_ALT).contains(1));
}

TEST(Shortcuts, DefaultsNeverClash) {
    auto list = shortcuts_from_json(default_keybinds());
    for (size_t i = 0; i < list.size(); ++i)
        list[i].id = int64_t(i + 1);
    EXPECT_TRUE(shortcut_clashes(list, WLR_MODIFIER_LOGO).empty());
    // With Alt as Mod, Mod+Tab (spaces) lands on Alt+Tab (windows): shown as a clash.
    for (const auto& [id, others] : shortcut_clashes(list, WLR_MODIFIER_ALT))
        EXPECT_TRUE(list[id - 1].keys.ends_with("Tab")) << list[id - 1].keys;
    list.push_back({999, list.front().keys, "close", ""});
    EXPECT_TRUE(shortcut_clashes(list, WLR_MODIFIER_LOGO).contains(999));
}

TEST(Shortcuts, BadOnesAreSkippedWithAReason) {
    std::vector<std::string> errors;
    const auto binds = resolve_keybinds(json::array({{{"keys", "Mod+B"}, {"action", "spawn"}, {"arg", "firefox"}},
                                                     {{"keys", "Mod+B"}, {"action", "fly"}},
                                                     {{"keys", "Mod+C"}, {"action", "spawn"}}}),
                                        WLR_MODIFIER_LOGO, &errors);
    ASSERT_EQ(binds.size(), 1u);
    EXPECT_EQ(binds[0].arg, "firefox");
    EXPECT_EQ(errors.size(), 2u);
}

TEST(Settings, StoresOnlyChangedValues) {
    Registry r(":memory:");
    {
        Settings s(Config::defaults(false), &r);
        ASSERT_FALSE(s.set("appearance.corner_radius", 12));  // the default
        ASSERT_FALSE(s.set("keyboard.repeat_rate", 50));
    }
    const auto stored = r.settings();
    EXPECT_EQ(stored.size(), 1u);
    EXPECT_EQ(stored.at("keyboard.repeat_rate"), 50);

    Settings reloaded(Config::defaults(false), &r);
    reloaded.load();
    EXPECT_EQ(reloaded.get("keyboard.repeat_rate"), 50);
    ASSERT_FALSE(reloaded.reset("keyboard.repeat_rate"));
    EXPECT_EQ(reloaded.get("keyboard.repeat_rate"), 35);
    EXPECT_TRUE(r.settings().empty());
}

TEST(Settings, BadStoredValuesNeverBlockStartup) {
    Registry r(":memory:");
    r.set_setting("appearance.corner_radius", 9999);
    r.set_setting("gone.key", 1);
    r.set_setting("keyboard.repeat_rate", 20);
    Settings s(Config::defaults(false), &r);
    s.load();
    EXPECT_EQ(s.get("appearance.corner_radius"), 12);  // rejected, default kept
    EXPECT_EQ(s.get("keyboard.repeat_rate"), 20);      // the rest still loads
    EXPECT_FALSE(r.settings().contains("gone.key"));   // and the dead ones are cleared
}

TEST(Settings, ImportsAnOldFile) {
    Registry r(":memory:");
    Settings s(Config::defaults(false), &r);
    s.import(json::parse(R"({"keyboard.repeat_rate": 44, "windows.rules": [], "nonsense": 1})"));
    EXPECT_EQ(s.get("keyboard.repeat_rate"), 44);
    EXPECT_EQ(r.settings().size(), 1u);
}

TEST(Settings, EverySchemaDefaultValidates) {
    Settings s(Config::defaults(false), nullptr);
    for (const auto& entry : s.schema())
        EXPECT_FALSE(s.set(entry.key, entry.default_value)) << entry.key;
}

TEST(Settings, TextLists) {
    Settings s(Config::defaults(false), nullptr);
    EXPECT_EQ(s.set("appearance.blurred_panels", json::array({"atrium-*", "waybar"})), std::nullopt);
    EXPECT_EQ(s.get("appearance.blurred_panels"), json::array({"atrium-*", "waybar"}));
    EXPECT_EQ(s.set("appearance.blurred_panels", json::array()), std::nullopt);
    EXPECT_NE(s.set("appearance.blurred_panels", json::array({"foot", 3})), std::nullopt);
    EXPECT_NE(s.set("appearance.blurred_panels", "foot"), std::nullopt);
}
