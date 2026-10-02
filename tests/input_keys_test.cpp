// input::Keys: a group of keyboards on one xkb state.
#include "input/keys.hpp"

#include <gtest/gtest.h>
#include <linux/input-event-codes.h>

#include <vector>

using atrium::input::Keys;
using atrium::input::Mod;

namespace {

xkb_keymap* keymap(const char* layouts = "us,de") {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names names{};
    names.layout = layouts;
    xkb_keymap* km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(ctx);
    return km;
}

struct Group {
    Keys keys;
    std::vector<std::pair<uint32_t, bool>> heard;
    int modifier_changes = 0;
    Group() {
        xkb_keymap* km = keymap();
        keys.set_keymap(km);
        xkb_keymap_unref(km);
        keys.on_key = [this](uint32_t, uint32_t k, bool p) { heard.emplace_back(k, p); };
        keys.on_modifiers = [this] { ++modifier_changes; };
    }
};

} // namespace

TEST(InputKeys, ShiftHeldCountsForTheNextKey) {
    Group g;
    g.keys.key(1, KEY_LEFTSHIFT, true);
    EXPECT_EQ(g.keys.mod_mask(), uint32_t(Mod::Shift));
    EXPECT_EQ(g.modifier_changes, 1);
    EXPECT_EQ(g.keys.sym_at(KEY_A, 1), XKB_KEY_A);
    g.keys.key(2, KEY_LEFTSHIFT, false);
    EXPECT_EQ(g.keys.mod_mask(), 0u);
}

// A binding sees the modifiers held with its key, not those it makes.
TEST(InputKeys, AKeyIsHeardBeforeTheStateChanges) {
    Group g;
    uint32_t seen = ~0u;
    g.keys.on_key = [&](uint32_t, uint32_t, bool) { seen = g.keys.mod_mask(); };
    g.keys.key(1, KEY_LEFTMETA, true);
    EXPECT_EQ(seen, 0u);
    EXPECT_EQ(g.keys.mod_mask(), uint32_t(Mod::Logo));
}

// The same key held on two keyboards: pressed once, released once, when the
// last one lets go.
TEST(InputKeys, AKeyOnTwoKeyboardsIsOnePress) {
    Group g;
    g.keys.key(1, KEY_LEFTCTRL, true);
    g.keys.key(2, KEY_LEFTCTRL, true);
    g.keys.key(3, KEY_LEFTCTRL, false);
    EXPECT_EQ(g.heard.size(), 1u);
    EXPECT_EQ(g.keys.mod_mask(), uint32_t(Mod::Ctrl));
    g.keys.key(4, KEY_LEFTCTRL, false);
    EXPECT_EQ(g.heard, (std::vector<std::pair<uint32_t, bool>>{{KEY_LEFTCTRL, true}, {KEY_LEFTCTRL, false}}));
    EXPECT_EQ(g.keys.mod_mask(), 0u);
    g.keys.key(5, KEY_LEFTCTRL, false);  // a release never pressed: nothing
    EXPECT_EQ(g.heard.size(), 2u);
}

TEST(InputKeys, LayoutsSwitchAndKeepModifiers) {
    Group g;
    g.keys.key(1, KEY_LEFTSHIFT, true);
    g.keys.set_layout(1);
    EXPECT_EQ(g.keys.layout(), 1u);
    EXPECT_EQ(g.keys.sym_at(KEY_Y, 0), XKB_KEY_z);  // German: Y and Z trade places
    EXPECT_EQ(g.keys.mod_mask(), uint32_t(Mod::Shift));
    g.keys.set_layout(0);
    EXPECT_EQ(g.keys.sym_at(KEY_Y, 0), XKB_KEY_y);
}

// A virtual keyboard says its modifiers; its keys don't make them.
TEST(InputKeys, AVirtualKeyboardSetsItsOwnModifiers) {
    Group g;
    g.keys.key(1, KEY_LEFTSHIFT, true, false);
    EXPECT_EQ(g.keys.mod_mask(), 0u);
    g.keys.set_modifiers({.depressed = 1});  // Shift
    EXPECT_EQ(g.keys.mod_mask(), uint32_t(Mod::Shift));
    EXPECT_EQ(g.keys.modifiers().depressed, 1u);
}

// A new keymap (settings changed) with a key held: still held in it.
TEST(InputKeys, ANewKeymapKeepsKeysHeld) {
    Group g;
    g.keys.key(1, KEY_LEFTALT, true);
    xkb_keymap* km = keymap("fr");
    g.keys.set_keymap(km);
    xkb_keymap_unref(km);
    EXPECT_EQ(g.keys.mod_mask(), uint32_t(Mod::Alt));
    EXPECT_EQ(g.keys.pressed(), (std::vector<uint32_t>{KEY_LEFTALT}));
}
