#pragma once
// The keyboard the system is set up for (`localectl set-x11-keymap`, or the
// installer), from /etc/X11/xorg.conf.d/00-keyboard.conf: used while atrium's
// own keyboard settings are left empty.

#include <string>
#include <string_view>

namespace atrium {

struct XkbNames {
    std::string model, layout, variant, options;
};

// `Option "XkbLayout" "de"` lines, wherever they are in the file.
XkbNames parse_x11_keyboard(std::string_view conf);

// The system's, read once; empty fields when it names none.
const XkbNames& system_keyboard();

} // namespace atrium
