#include "../src/x11_listen_core.hpp"

#include <gtest/gtest.h>

#include <xkbcommon/xkbcommon-keysyms.h>

using namespace atrium::x11_listen;

TEST(X11Listen, KWinsModes) {
    // Never: nothing.
    EXPECT_FALSE(hears("never", XKB_KEY_Control_L, false));
    // Modifier keys: Ctrl, Super, F-keys, media keys; not letters, even with Ctrl.
    EXPECT_TRUE(hears("modifier_keys", XKB_KEY_Control_L, false));
    EXPECT_TRUE(hears("modifier_keys", XKB_KEY_Super_L, false));
    EXPECT_TRUE(hears("modifier_keys", XKB_KEY_F13, false));
    EXPECT_TRUE(hears("modifier_keys", XKB_KEY_XF86AudioPlay, false));
    EXPECT_FALSE(hears("modifier_keys", XKB_KEY_a, true));
    EXPECT_FALSE(hears("modifier_keys", XKB_KEY_KP_5, false));
    // Input-method and dead keys stay private (KWin's list).
    EXPECT_FALSE(hears("modifier_keys", XKB_KEY_Henkan, false));
    EXPECT_FALSE(hears("modifier_keys", XKB_KEY_dead_acute, false));
    EXPECT_FALSE(hears("modifier_keys", XKB_KEY_Multi_key, false));
    // Shortcuts: also any key with Ctrl, Alt or Super held.
    EXPECT_TRUE(hears("shortcuts", XKB_KEY_a, true));
    EXPECT_FALSE(hears("shortcuts", XKB_KEY_a, false));
    // Always: typing too.
    EXPECT_TRUE(hears("always", XKB_KEY_a, false));
}
