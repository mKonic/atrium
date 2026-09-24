#include "keyboard_core.hpp"

#include <gtest/gtest.h>

using namespace atrium::keyboard;

TEST(XkbList, LayoutsAndVariants) {
    const char* text = "! model\n  pc105           Generic 105-key PC\n\n"
                       "! layout\n  us              English (US)\n  de              German\n\n"
                       "! variant\n  nodeadkeys      de: German (no dead keys)\n  intl            us: English (US, intl., with dead keys)\n\n"
                       "! option\n  grp                  Switching to another layout\n";
    const XkbList l = parse_xkb_list(text);
    ASSERT_EQ(l.layouts.size(), 2u);
    EXPECT_EQ(l.layouts[0].code, "us");
    EXPECT_EQ(l.layouts[1].description, "German");
    ASSERT_EQ(l.variants.size(), 2u);
    EXPECT_EQ(l.variants[0].layout, "de");
    EXPECT_EQ(l.variants[0].code, "nodeadkeys");
    EXPECT_EQ(l.variants[1].description, "English (US, intl., with dead keys)");
}

TEST(XkbList, SkipsJunk) {
    const XkbList l = parse_xkb_list("! layout\n\n  lonely\n  us   English (US)");
    ASSERT_EQ(l.layouts.size(), 1u);
    EXPECT_EQ(l.layouts[0].code, "us");
}

#include "keyboard_conf.hpp"

TEST(X11Keyboard, ReadsLocaledsFile) {
    const char* conf = "# Written by systemd-localed(8)\nSection \"InputClass\"\n"
                       "        Identifier \"system-keyboard\"\n        MatchIsKeyboard \"on\"\n"
                       "        Option \"XkbLayout\" \"de,us\"\n        Option \"XkbVariant\" \"nodeadkeys,\"\n"
                       "        Option \"XkbOptions\" \"grp:alt_shift_toggle\"\n#       Option \"XkbModel\" \"pc105\"\nEndSection\n";
    const atrium::XkbNames n = atrium::parse_x11_keyboard(conf);
    EXPECT_EQ(n.layout, "de,us");
    EXPECT_EQ(n.variant, "nodeadkeys,");
    EXPECT_EQ(n.options, "grp:alt_shift_toggle");
    EXPECT_EQ(n.model, "");
    EXPECT_EQ(atrium::parse_x11_keyboard("").layout, "");
}
