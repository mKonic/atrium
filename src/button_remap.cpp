#include "button_remap.hpp"

#include <sstream>

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_keyboard.h>

namespace atrium::button_remap {

namespace {

// "Ctrl+Shift" → the modifier bits; nothing for a word that isn't one.
std::optional<uint32_t> parse_mods(const std::string& text) {
    uint32_t mods = 0;
    std::stringstream in(text);
    std::string part;
    while (std::getline(in, part, '+')) {
        if (part.empty())
            continue;
        const uint32_t bit = modifier_from_name(part);
        if (!bit)
            return std::nullopt;
        mods |= bit;
    }
    return mods;
}

bool is_button(uint32_t code) {
    return code >= BTN_MOUSE && code < BTN_JOYSTICK;
}

} // namespace

std::map<uint32_t, Remap> parse(const json& v, std::vector<std::string>* errors) {
    std::map<uint32_t, Remap> out;
    auto fail = [errors](const std::string& why) {
        if (errors)
            errors->push_back(why);
    };
    if (!v.is_array()) {
        fail("pointer.buttons is a list");
        return out;
    }
    for (const json& e : v) {
        if (!e.is_object() || !e.contains("button") || !e["button"].is_number_unsigned()) {
            fail("each remap has a button");
            continue;
        }
        const uint32_t button = e["button"].get<uint32_t>();
        if (!is_button(button)) {
            fail(std::to_string(button) + " isn't a mouse button");
            continue;
        }
        if (button == BTN_LEFT) {
            fail("the left button can't be changed");
            continue;
        }
        const std::string action = e.value("action", "");
        Remap r;
        if (action == "keys") {
            const auto chord = parse_chord(e.value("keys", ""));
            if (!chord || chord->uses_mod) {
                fail(button_name(button) + ": no such keys \"" + e.value("keys", "") + "\"");
                continue;
            }
            r.kind = Remap::Keys;
            r.mods = chord->mods;
            r.sym = chord->sym;
        } else if (action == "button") {
            const uint32_t to = e.value("to", 0u);
            const auto mods = parse_mods(e.value("modifiers", ""));
            if (!is_button(to) || !mods) {
                fail(button_name(button) + ": no such button or modifiers");
                continue;
            }
            r.kind = Remap::Button;
            r.to = to;
            r.mods = *mods;
        } else if (action == "disabled") {
            r.kind = Remap::Disabled;
        } else {
            fail(button_name(button) + ": no action \"" + action + "\"");
            continue;
        }
        out[button] = r;
    }
    return out;
}

std::vector<uint32_t> modifier_keys(uint32_t mods) {
    std::vector<uint32_t> out;
    if (mods & WLR_MODIFIER_SHIFT)
        out.push_back(KEY_LEFTSHIFT);
    if (mods & WLR_MODIFIER_CTRL)
        out.push_back(KEY_LEFTCTRL);
    if (mods & WLR_MODIFIER_ALT)
        out.push_back(KEY_LEFTALT);
    if (mods & WLR_MODIFIER_LOGO)
        out.push_back(KEY_LEFTMETA);
    return out;
}

std::vector<uint32_t> keys_for(xkb_keymap* keymap, xkb_layout_index_t layout, uint32_t chord_mods, xkb_keysym_t sym) {
    if (!keymap)
        return {};
    for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap); code <= xkb_keymap_max_keycode(keymap); ++code)
        for (xkb_level_index_t level = 0; level < 2; ++level) {
            const xkb_keysym_t* syms = nullptr;
            const int n = xkb_keymap_key_get_syms_by_level(keymap, code, layout, level, &syms);
            if (n != 1 || xkb_keysym_to_lower(syms[0]) != sym)
                continue;
            uint32_t mods = chord_mods;
            if (level == 1 && syms[0] == sym)
                mods |= WLR_MODIFIER_SHIFT;  // only on the shifted level
            std::vector<uint32_t> out = modifier_keys(mods);
            out.push_back(code - 8);
            return out;
        }
    return {};
}

} // namespace atrium::button_remap
