#pragma once
// Mouse buttons doing something else, as KWin's buttonrebinds plugin
// rebinds them (Settings > Mouse > Extra mouse buttons): a button presses a
// key combination, acts as another button (with modifiers held), or does
// nothing. Settings JSON, `pointer.buttons`:
//
//   [{"button": 275, "action": "keys", "keys": "Ctrl+Shift+T"},
//    {"button": 276, "action": "button", "to": 274, "modifiers": "Ctrl"},
//    {"button": 277, "action": "disabled"}]
//
// Buttons by evdev code (BTN_SIDE 275, BTN_EXTRA 276, ...). The left button
// can't be changed: there'd be no way left to click.

#include "button_names.hpp"
#include "settings.hpp"

#include <map>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

namespace atrium::button_remap {

using Remap = ButtonRemap;

// Each button's remap; entries that don't parse are left out with the
// reason in `errors`.
std::map<uint32_t, Remap> parse(const json& v, std::vector<std::string>* errors = nullptr);

// The evdev keys that type `chord` in `layout` of `keymap`: its modifiers
// (left Shift, Ctrl, Alt, Super) and then the key itself; Shift too when
// the symbol is on the shifted level. Empty when no key types it.
std::vector<uint32_t> keys_for(xkb_keymap* keymap, xkb_layout_index_t layout, uint32_t mods, xkb_keysym_t sym);

// The evdev keys for modifiers alone.
std::vector<uint32_t> modifier_keys(uint32_t mods);

} // namespace atrium::button_remap
