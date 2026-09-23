#include "settings.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace atrium;
namespace fs = std::filesystem;

namespace {

fs::path temp_file(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("atrium-test-") + name + "-" +
                                               std::to_string(::getpid()) + ".json");
    fs::remove(p);
    return p;
}

} // namespace

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
    Settings s(Config::defaults(false), temp_file("defaults"));
    EXPECT_EQ(s.get("appearance.corner_radius"), 12);
    EXPECT_EQ(s.get("shortcuts.modifier"), "super");
    EXPECT_EQ(Settings(Config::defaults(true), temp_file("nested")).get("shortcuts.modifier"), "alt");
}

TEST(Settings, ValidatesAgainstTheSchema) {
    Settings s(Config::defaults(false), temp_file("validate"));
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
    Settings s(Config::defaults(false), temp_file("floatint"));
    EXPECT_FALSE(s.set("appearance.corner_radius", 16.0));
    EXPECT_TRUE(s.get("appearance.corner_radius").is_number_integer());
    EXPECT_TRUE(s.set("appearance.corner_radius", 16.5));
}

TEST(Settings, AppliesToTheConfig) {
    Settings s(Config::defaults(false), temp_file("apply"));
    ASSERT_FALSE(s.set("appearance.corner_radius", 3));
    ASSERT_FALSE(s.set("pointer.acceleration", "flat"));
    ASSERT_FALSE(s.set("appearance.shadow_color", "#ff0000"));
    ASSERT_FALSE(s.set("shortcuts.modifier", "alt"));
    Config c = Config::defaults(false);
    s.apply(c);
    EXPECT_EQ(c.corner_radius, 3);
    EXPECT_EQ(c.accel_profile, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
    EXPECT_FLOAT_EQ(c.shadow_color[0], 1.0f);
    // Bindings written with Mod follow the modifier setting.
    EXPECT_NE(find_keybind(c.keybinds, WLR_MODIFIER_ALT, XKB_KEY_q), nullptr);
    EXPECT_EQ(find_keybind(c.keybinds, WLR_MODIFIER_LOGO, XKB_KEY_q), nullptr);
}

TEST(Settings, CustomBindings) {
    Settings s(Config::defaults(false), temp_file("binds"));
    json binds = json::array({{{"keys", "Mod+B"}, {"action", "spawn"}, {"arg", "firefox"}}});
    ASSERT_FALSE(s.set("shortcuts.bindings", binds));
    Config c = Config::defaults(false);
    s.apply(c);
    ASSERT_EQ(c.keybinds.size(), 1u);
    EXPECT_EQ(c.keybinds[0].arg, "firefox");

    EXPECT_TRUE(s.set("shortcuts.bindings", json::array({{{"keys", "Mod+B"}, {"action", "fly"}}})));
    EXPECT_TRUE(s.set("shortcuts.bindings", json::array({{{"keys", "Mod+B"}, {"action", "spawn"}}})));
    EXPECT_TRUE(s.set("shortcuts.bindings", "Mod+B"));
}

TEST(Settings, StoresOnlyChangedValues) {
    const fs::path file = temp_file("store");
    {
        Settings s(Config::defaults(false), file);
        ASSERT_FALSE(s.set("appearance.corner_radius", 12));  // the default
        ASSERT_FALSE(s.set("keyboard.repeat_rate", 50));
        ASSERT_TRUE(s.save());
    }
    std::ifstream in(file);
    json doc = json::parse(in);
    EXPECT_EQ(doc.size(), 1u);
    EXPECT_EQ(doc["keyboard.repeat_rate"], 50);

    Settings reloaded(Config::defaults(false), file);
    ASSERT_TRUE(reloaded.load());
    EXPECT_EQ(reloaded.get("keyboard.repeat_rate"), 50);
    ASSERT_FALSE(reloaded.reset("keyboard.repeat_rate"));
    EXPECT_EQ(reloaded.get("keyboard.repeat_rate"), 35);
    fs::remove(file);
}

TEST(Settings, BadStoredValuesNeverBlockStartup) {
    const fs::path file = temp_file("bad");
    std::ofstream(file) << R"({"appearance.corner_radius": 9999, "gone.key": 1, "keyboard.repeat_rate": 20})";
    Settings s(Config::defaults(false), file);
    EXPECT_TRUE(s.load());
    EXPECT_EQ(s.get("appearance.corner_radius"), 12);  // rejected, default kept
    EXPECT_EQ(s.get("keyboard.repeat_rate"), 20);      // the rest still loads
    std::ofstream(file) << "{ not json";
    Settings broken(Config::defaults(false), file);
    EXPECT_FALSE(broken.load());
    EXPECT_EQ(broken.get("keyboard.repeat_rate"), 35);
    fs::remove(file);
}

TEST(Settings, MissingFileIsFine) {
    Settings s(Config::defaults(false), temp_file("missing"));
    EXPECT_TRUE(s.load());
}

TEST(Settings, EverySchemaDefaultValidates) {
    Settings s(Config::defaults(false), temp_file("schema"));
    for (const auto& entry : s.schema())
        EXPECT_FALSE(s.set(entry.key, entry.default_value)) << entry.key;
}
