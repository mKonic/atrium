#include "config.hpp"

namespace atrium {

Config Config::defaults(bool nested) {
    Config c;
    c.mod = nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
    const uint32_t M = c.mod, S = WLR_MODIFIER_SHIFT;

    c.keybinds = {
        {M,     XKB_KEY_Return, Action::SpawnTerminal},
        {M,     XKB_KEY_q,      Action::CloseWindow},
        {M,     XKB_KEY_f,      Action::ToggleFullscreen},
        {M,     XKB_KEY_Up,     Action::ToggleMaximize},
        {M,     XKB_KEY_h,      Action::Minimize},
        {M,     XKB_KEY_Tab,    Action::FocusNext},
        {M | S, XKB_KEY_Tab,    Action::FocusPrev},
        {M | S, XKB_KEY_e,      Action::Quit},
    };

    // Ctrl+Alt+Fn switches VT; the keysym is XF86Switch_VT_n on most keymaps.
    constexpr uint32_t CA = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT;
    for (int vt = 1; vt <= 12; ++vt)
        c.keybinds.push_back({CA, xkb_keysym_t(XKB_KEY_XF86Switch_VT_1 + vt - 1),
                              Action::SwitchVt, {}, vt});
    return c;
}

} // namespace atrium
