#include "x11_listen_core.hpp"

namespace atrium::x11_listen {

bool non_character(uint32_t s) {
    // Keypad keys that type (digits, operators, space) count as characters, as Qt maps them.
    if ((s >= 0xffb0 && s <= 0xffb9) || (s >= 0xffaa && s <= 0xffaf) || s == 0xffbd || s == 0xff80)
        return false;
    // Composing and input-method keys, and dead keys (KWin's excluded list).
    if (s == 0xff20 || s == 0xff37 || s == 0xff7e || (s >= 0xff21 && s <= 0xff3f) || (s >= 0xfe50 && s <= 0xfe93))
        return false;
    // Function, cursor, modifier and other keys that type nothing; XF86 and
    // other vendor keys (media, brightness).
    return (s >= 0xff00 && s <= 0xffff) || (s >= 0x10080000 && s <= 0x1008ffff);
}

bool hears(std::string_view mode, uint32_t keysym, bool ctrl_alt_super) {
    if (mode == "always")
        return true;
    if (mode == "shortcuts")
        return ctrl_alt_super || non_character(keysym);
    if (mode == "modifier_keys")
        return non_character(keysym);
    return false;
}

} // namespace atrium::x11_listen
