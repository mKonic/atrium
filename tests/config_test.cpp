#include "config.hpp"

#include <gtest/gtest.h>

using namespace atrium;

TEST(Keybinds, MatchesModAndKey) {
    Config c = Config::defaults(false);
    const Keybind* b = find_keybind(c.keybinds, WLR_MODIFIER_LOGO, XKB_KEY_q);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->action, Action::CloseWindow);
}

TEST(Keybinds, IgnoresCapsAndNumLock) {
    Config c = Config::defaults(false);
    const uint32_t locks = WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2;
    const Keybind* b = find_keybind(c.keybinds, WLR_MODIFIER_LOGO | locks, XKB_KEY_q);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->action, Action::CloseWindow);
}

TEST(Keybinds, ExtraModifiersDoNotMatch) {
    Config c = Config::defaults(false);
    EXPECT_EQ(find_keybind(c.keybinds, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, XKB_KEY_q), nullptr);
    EXPECT_EQ(find_keybind(c.keybinds, 0, XKB_KEY_q), nullptr);
}

TEST(Keybinds, ShiftSelectsTheShiftedBinding) {
    Config c = Config::defaults(false);
    const Keybind* next = find_keybind(c.keybinds, WLR_MODIFIER_LOGO, XKB_KEY_Tab);
    const Keybind* prev = find_keybind(c.keybinds, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Tab);
    ASSERT_NE(next, nullptr);
    ASSERT_NE(prev, nullptr);
    EXPECT_EQ(next->action, Action::CycleSpaceNext);
    EXPECT_EQ(prev->action, Action::CycleSpacePrev);
    const Keybind* sw = find_keybind(c.keybinds, WLR_MODIFIER_ALT, XKB_KEY_Tab);
    const Keybind* sw_back = find_keybind(c.keybinds, WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT, XKB_KEY_Tab);
    ASSERT_NE(sw, nullptr);
    ASSERT_NE(sw_back, nullptr);
    EXPECT_EQ(sw->action, Action::SwitchNext);
    EXPECT_EQ(sw_back->action, Action::SwitchPrev);
}

TEST(Config, NestedUsesAltAsTheModifier) {
    EXPECT_EQ(Config::defaults(true).mod, uint32_t(WLR_MODIFIER_ALT));
    EXPECT_EQ(Config::defaults(false).mod, uint32_t(WLR_MODIFIER_LOGO));
    const Config nested = Config::defaults(true);
    EXPECT_NE(find_keybind(nested.keybinds, WLR_MODIFIER_ALT, XKB_KEY_Return), nullptr);
    EXPECT_EQ(find_keybind(nested.keybinds, WLR_MODIFIER_LOGO, XKB_KEY_Return), nullptr);
}

TEST(Config, EveryVtHasABinding) {
    Config c = Config::defaults(false);
    for (int vt = 1; vt <= 12; ++vt) {
        const Keybind* b = find_keybind(c.keybinds, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
                                        XKB_KEY_F1 + vt - 1);
        ASSERT_NE(b, nullptr) << "vt " << vt;
        EXPECT_EQ(b->action, Action::SwitchVt);
        EXPECT_EQ(b->iarg, vt);
    }
}

// The seat matches bindings against a key's level-0 and level-1 symbols. On a
// real keymap XF86Switch_VT_n lives on a higher level of the F keys, so a VT
// binding written with it never fires; F1-F12 do.
TEST(Config, VtBindingsMatchWhatTheKeymapProduces) {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_rule_names names{};
    names.layout = "us";
    xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    ASSERT_NE(keymap, nullptr);

    Config c = Config::defaults(false);
    const xkb_keycode_t f1 = 59 + 8;  // KEY_F1, evdev → xkb
    bool matched = false;
    for (xkb_level_index_t level = 0; level < 2 && !matched; ++level) {
        const xkb_keysym_t* syms;
        if (xkb_keymap_key_get_syms_by_level(keymap, f1, 0, level, &syms) > 0)
            matched = find_keybind(c.keybinds, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, syms[0]) != nullptr;
    }
    EXPECT_TRUE(matched);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
}
