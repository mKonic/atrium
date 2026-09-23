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
    EXPECT_EQ(next->action, Action::FocusNext);
    EXPECT_EQ(prev->action, Action::FocusPrev);
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
                                        XKB_KEY_XF86Switch_VT_1 + vt - 1);
        ASSERT_NE(b, nullptr) << "vt " << vt;
        EXPECT_EQ(b->iarg, vt);
    }
}
