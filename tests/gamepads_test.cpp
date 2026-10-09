#include "../src/gamepads.hpp"

#include <gtest/gtest.h>
#include <linux/input.h>

using atrium::is_game_controller;

namespace {

constexpr size_t kBits = sizeof(unsigned long) * 8;

struct Caps {
    unsigned long abs[ABS_CNT / kBits + 1] = {};
    unsigned long key[KEY_CNT / kBits + 1] = {};
    Caps& with_abs(unsigned c) { abs[c / kBits] |= 1UL << (c % kBits); return *this; }
    Caps& with_key(unsigned c) { key[c / kBits] |= 1UL << (c % kBits); return *this; }
};

} // namespace

TEST(Gamepads, SticksOrADpadMakeAController) {
    EXPECT_TRUE(is_game_controller(Caps().with_abs(ABS_X).abs, Caps().key));
    EXPECT_TRUE(is_game_controller(Caps().with_abs(ABS_HAT0X).abs, Caps().key));
}

TEST(Gamepads, GamepadOrJoystickButtonsMakeAController) {
    EXPECT_TRUE(is_game_controller(Caps().abs, Caps().with_key(BTN_GAMEPAD).key));
    EXPECT_TRUE(is_game_controller(Caps().abs, Caps().with_key(BTN_JOYSTICK).key));
}

TEST(Gamepads, AKeyboardIsNot) {
    Caps c;
    c.with_key(KEY_A).with_key(KEY_ENTER).with_abs(ABS_MISC);
    EXPECT_FALSE(is_game_controller(c.abs, c.key));
}
