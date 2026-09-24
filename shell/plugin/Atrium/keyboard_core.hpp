#pragma once
// xkeyboard-config's list of layouts (rules/evdev.lst): each layout's code
// and name, and each variant with the layout it belongs to.

#include <string>
#include <string_view>
#include <vector>

namespace atrium::keyboard {

struct Layout {
    std::string code;         // "de"
    std::string description;  // "German"
};

struct Variant {
    std::string layout;       // "de"
    std::string code;         // "nodeadkeys"
    std::string description;  // "German (no dead keys)"
};

struct XkbList {
    std::vector<Layout> layouts;
    std::vector<Variant> variants;
};

// In the file's order.
XkbList parse_xkb_list(std::string_view text);

} // namespace atrium::keyboard
