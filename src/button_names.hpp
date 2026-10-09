#pragma once
// Mouse buttons by name, for remapping them: by their evdev codes, and from
// Qt's (as QtWayland maps them: BTN_SIDE is Qt's first extra button).

#include <cstdint>
#include <string>

#include <linux/input-event-codes.h>

namespace atrium::button_remap {

// "Back", "Forward", "Middle", "Button 9".
inline std::string button_name(uint32_t code) {
    switch (code) {
    case BTN_LEFT: return "Left";
    case BTN_RIGHT: return "Right";
    case BTN_MIDDLE: return "Middle";
    // Most mice: the thumb buttons, "back" nearer the palm.
    case BTN_SIDE: return "Back";
    case BTN_EXTRA: return "Forward";
    default: return "Button " + std::to_string(code - BTN_MOUSE + 1);
    }
}

// Qt::MouseButton → evdev: Left, Right, Middle, then ExtraButton1 (BackButton)
// is BTN_SIDE and each one after the next code. 0 for none.
inline uint32_t evdev_button(uint32_t qt) {
    if (qt == 0x1) return BTN_LEFT;
    if (qt == 0x2) return BTN_RIGHT;
    if (qt == 0x4) return BTN_MIDDLE;
    if (qt < 0x8 || (qt & (qt - 1)))
        return 0;
    uint32_t code = BTN_SIDE;
    for (uint32_t b = 0x8; b < qt; b <<= 1)
        ++code;
    return code < BTN_JOYSTICK ? code : 0;
}

} // namespace atrium::button_remap
