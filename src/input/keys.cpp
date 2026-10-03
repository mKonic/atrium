#include "input/keys.hpp"

#include <algorithm>  // std::erase

namespace atrium::input {

namespace {

// The xkb names of the Mod bits, in their order.
constexpr const char* kModNames[8] = {XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT,
                                      "Mod2",             "Mod3",            XKB_MOD_NAME_LOGO, "Mod5"};

} // namespace

Keys::Keys() = default;

Keys::~Keys() {
    xkb_state_unref(state_);
    xkb_keymap_unref(keymap_);
}

void Keys::set_keymap(xkb_keymap* keymap) {
    if (!keymap)
        return;
    xkb_keymap_ref(keymap);
    xkb_state_unref(state_);
    xkb_keymap_unref(keymap_);
    keymap_ = keymap;
    state_ = xkb_state_new(keymap);
    for (int i = 0; i < 8; ++i)
        mod_index_[i] = xkb_keymap_mod_get_index(keymap, kModNames[i]);
    // Keys still held count in the new state too.
    for (uint32_t k : pressed_)
        xkb_state_update_key(state_, k + 8, XKB_KEY_DOWN);
    refresh_modifiers();
}

void Keys::key(uint32_t time_ms, uint32_t keycode, bool pressed, bool update_state) {
    int& count = counts_[keycode];
    if (pressed) {
        if (count++ > 0)
            return;  // another keyboard holds it already
        pressed_.push_back(keycode);
    } else {
        if (count == 0)
            return;  // never seen pressed
        if (--count > 0)
            return;
        std::erase(pressed_, keycode);
    }
    // Heard before the state changes, as wlroots has it: a binding sees the
    // modifiers held with the key, not those the key itself makes.
    if (on_key)
        on_key(time_ms, keycode, pressed);
    if (update_state && state_) {
        xkb_state_update_key(state_, keycode + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
        refresh_modifiers();
    }
}

void Keys::set_modifiers(const Modifiers& m) {
    if (!state_)
        return;
    xkb_state_update_mask(state_, m.depressed, m.latched, m.locked, 0, 0, m.group);
    refresh_modifiers();
}

void Keys::set_layout(uint32_t index) {
    if (!state_)
        return;
    xkb_state_update_mask(state_, mods_.depressed, mods_.latched, mods_.locked, 0, 0, index);
    refresh_modifiers();
}

uint32_t Keys::layout() const {
    return state_ ? xkb_state_serialize_layout(state_, XKB_STATE_LAYOUT_EFFECTIVE) : 0;
}

uint32_t Keys::mod_mask() const {
    if (!state_)
        return 0;
    uint32_t mask = 0;
    for (int i = 0; i < 8; ++i)
        if (mod_index_[i] != XKB_MOD_INVALID &&
            xkb_state_mod_index_is_active(state_, mod_index_[i], XKB_STATE_MODS_EFFECTIVE) > 0)
            mask |= 1u << i;
    return mask;
}

xkb_keysym_t Keys::sym_at(uint32_t keycode, xkb_level_index_t level) const {
    if (!state_)
        return XKB_KEY_NoSymbol;
    const xkb_keycode_t code = keycode + 8;
    const xkb_layout_index_t layout = xkb_state_key_get_layout(state_, code);
    const xkb_keysym_t* syms;
    const int n = xkb_keymap_key_get_syms_by_level(keymap_, code, layout, level, &syms);
    return n ? syms[0] : XKB_KEY_NoSymbol;
}

void Keys::refresh_modifiers() {
    const Modifiers now{xkb_state_serialize_mods(state_, XKB_STATE_MODS_DEPRESSED),
                        xkb_state_serialize_mods(state_, XKB_STATE_MODS_LATCHED),
                        xkb_state_serialize_mods(state_, XKB_STATE_MODS_LOCKED),
                        xkb_state_serialize_layout(state_, XKB_STATE_LAYOUT_EFFECTIVE)};
    if (now == mods_)
        return;
    mods_ = now;
    if (on_modifiers)
        on_modifiers();
}

std::optional<KeyFor> key_for_keysym(xkb_keymap* keymap, xkb_layout_index_t layout, xkb_keysym_t sym) {
    if (!keymap)
        return std::nullopt;
    const xkb_mod_index_t shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    std::optional<KeyFor> shifted;
    for (xkb_keycode_t kc = xkb_keymap_min_keycode(keymap); kc <= xkb_keymap_max_keycode(keymap); ++kc) {
        if (kc < 8 || layout >= xkb_keymap_num_layouts_for_key(keymap, kc))
            continue;
        const xkb_level_index_t levels = xkb_keymap_num_levels_for_key(keymap, kc, layout);
        for (xkb_level_index_t level = 0; level < levels; ++level) {
            const xkb_keysym_t* syms = nullptr;
            const int n = xkb_keymap_key_get_syms_by_level(keymap, kc, layout, level, &syms);
            if (n != 1 || syms[0] != sym)
                continue;
            // The modifiers that reach this level: none, or Shift alone.
            xkb_mod_mask_t masks[8];
            const size_t m = xkb_keymap_key_get_mods_for_level(keymap, kc, layout, level, masks, 8);
            for (size_t i = 0; i < m; ++i) {
                if (masks[i] == 0)
                    return KeyFor{kc - 8, false};
                if (shift != XKB_MOD_INVALID && masks[i] == (1u << shift) && !shifted)
                    shifted = KeyFor{kc - 8, true};
            }
        }
    }
    return shifted;
}

} // namespace atrium::input
