#include "button_remap.hpp"

#include <gtest/gtest.h>
#include <linux/input-event-codes.h>

using namespace atrium;
using namespace atrium::button_remap;

TEST(ButtonRemap, ReadsEachAction) {
    std::vector<std::string> errors;
    const auto m = parse(json::parse(R"([
        {"button": 275, "action": "keys", "keys": "Ctrl+Shift+T"},
        {"button": 276, "action": "button", "to": 274, "modifiers": "Ctrl"},
        {"button": 277, "action": "disabled"}])"), &errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(m.size(), 3u);
    EXPECT_EQ(m.at(BTN_SIDE).kind, Remap::Keys);
    EXPECT_EQ(m.at(BTN_SIDE).mods, uint32_t(WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT));
    EXPECT_EQ(m.at(BTN_SIDE).sym, uint32_t(XKB_KEY_t));
    EXPECT_EQ(m.at(BTN_EXTRA).kind, Remap::Button);
    EXPECT_EQ(m.at(BTN_EXTRA).to, uint32_t(BTN_MIDDLE));
    EXPECT_EQ(m.at(BTN_EXTRA).mods, uint32_t(WLR_MODIFIER_CTRL));
    EXPECT_EQ(m.at(BTN_FORWARD).kind, Remap::Disabled);
}

TEST(ButtonRemap, WrongEntriesSaySo) {
    auto error = [](const char* text) {
        std::vector<std::string> errors;
        parse(json::parse(text), &errors);
        return errors.empty() ? std::string() : errors.front();
    };
    EXPECT_EQ(error(R"([{"button": 272, "action": "disabled"}])"), "the left button can't be changed");
    EXPECT_EQ(error(R"([{"button": 30, "action": "disabled"}])"), "30 isn't a mouse button");  // a key
    EXPECT_EQ(error(R"([{"action": "disabled"}])"), "each remap has a button");
    EXPECT_EQ(error(R"([{"button": 275, "action": "keys", "keys": "Ctrl+Nope"}])"), "Back: no such keys \"Ctrl+Nope\"");
    EXPECT_EQ(error(R"([{"button": 275, "action": "keys", "keys": "Mod+T"}])"), "Back: no such keys \"Mod+T\"");
    EXPECT_EQ(error(R"([{"button": 275, "action": "button", "to": 30}])"), "Back: no such button or modifiers");
    EXPECT_EQ(error(R"([{"button": 275, "action": "button", "to": 273, "modifiers": "Hyper"}])"), "Back: no such button or modifiers");
    EXPECT_EQ(error(R"([{"button": 275, "action": "fly"}])"), "Back: no action \"fly\"");
    EXPECT_EQ(error(R"({"button": 275})"), "pointer.buttons is a list");
    // A bad entry leaves the good ones.
    EXPECT_EQ(parse(json::parse(R"([{"button": 272, "action": "disabled"}, {"button": 275, "action": "disabled"}])")).size(), 1u);
}

TEST(ButtonRemap, Names) {
    EXPECT_EQ(button_name(BTN_SIDE), "Back");
    EXPECT_EQ(button_name(BTN_EXTRA), "Forward");
    EXPECT_EQ(button_name(BTN_MIDDLE), "Middle");
    EXPECT_EQ(button_name(0x118), "Button 9");
    EXPECT_EQ(button_name(BTN_FORWARD), "Button 6");
    EXPECT_EQ(evdev_button(0x1), uint32_t(BTN_LEFT));
    EXPECT_EQ(evdev_button(0x4), uint32_t(BTN_MIDDLE));
    EXPECT_EQ(evdev_button(0x8), uint32_t(BTN_SIDE));      // Qt::BackButton
    EXPECT_EQ(evdev_button(0x10), uint32_t(BTN_EXTRA));    // Qt::ForwardButton
    EXPECT_EQ(evdev_button(0x40), uint32_t(BTN_BACK));     // ExtraButton4
    EXPECT_EQ(evdev_button(0x18), 0u);                     // two at once
    EXPECT_EQ(evdev_button(0), 0u);
}

TEST(ButtonRemap, KeysInTheLayout) {
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const xkb_rule_names names{.rules = "evdev", .model = "pc105", .layout = "us", .variant = "", .options = ""};
    xkb_keymap* keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    ASSERT_NE(keymap, nullptr);
    // Modifiers first, then the key.
    EXPECT_EQ(keys_for(keymap, 0, WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT, XKB_KEY_t),
              (std::vector<uint32_t>{KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_T}));
    // A shifted symbol brings Shift along.
    EXPECT_EQ(keys_for(keymap, 0, WLR_MODIFIER_CTRL, XKB_KEY_question),
              (std::vector<uint32_t>{KEY_LEFTSHIFT, KEY_LEFTCTRL, KEY_SLASH}));
    EXPECT_EQ(keys_for(keymap, 0, 0, XKB_KEY_XF86AudioPlay), (std::vector<uint32_t>{KEY_PLAYPAUSE}));
    EXPECT_TRUE(keys_for(keymap, 0, 0, XKB_KEY_Cyrillic_a).empty());  // not in this layout
    EXPECT_TRUE(keys_for(nullptr, 0, 0, XKB_KEY_t).empty());
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
}
