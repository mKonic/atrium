#pragma once
// A group of keyboards sharing one xkb state: Shift held on one counts on
// the others, as on one keyboard. Every physical keyboard feeds one group;
// each virtual keyboard (an IME's, an on-screen one) has its own.
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace atrium::input {

// Modifier bits, as bindings and wlroots spell them (WLR_MODIFIER_*).
enum Mod : uint32_t {
    Shift = 1 << 0,
    Caps = 1 << 1,
    Ctrl = 1 << 2,
    Alt = 1 << 3,
    Mod2 = 1 << 4,
    Mod3 = 1 << 5,
    Logo = 1 << 6,
    Mod5 = 1 << 7,
};

// xkb's serialized state, as wl_keyboard sends it.
struct Modifiers {
    uint32_t depressed = 0, latched = 0, locked = 0, group = 0;
    bool operator==(const Modifiers&) const = default;
};

class Keys {
public:
    Keys();
    ~Keys();
    Keys(const Keys&) = delete;
    Keys& operator=(const Keys&) = delete;

    // Takes a reference. A new keymap starts a new state; keys held stay held.
    void set_keymap(xkb_keymap* keymap);
    xkb_keymap* keymap() const { return keymap_; }
    xkb_state* state() const { return state_; }

    void set_repeat(int32_t rate, int32_t delay) {
        repeat_rate = rate;
        repeat_delay = delay;
    }
    int32_t repeat_rate = 25, repeat_delay = 600;

    // A key (evdev code) on one of the group's keyboards. The group hears a
    // press only when no other keyboard holds the key already, and a release
    // only when none does any more. `update_state` false: the keyboard sends
    // its modifiers itself (a virtual keyboard).
    void key(uint32_t time_ms, uint32_t keycode, bool pressed, bool update_state = true);
    // Modifiers as a virtual keyboard (or a nested host) says they are.
    void set_modifiers(const Modifiers& mods);
    // The layout in use (from 0), keeping the rest of the modifiers.
    void set_layout(uint32_t index);
    uint32_t layout() const;

    const Modifiers& modifiers() const { return mods_; }
    // Mod bits held now (effective: latched and locked count).
    uint32_t mod_mask() const;
    // Evdev codes held, in the order pressed.
    const std::vector<uint32_t>& pressed() const { return pressed_; }
    // The symbol a key gives at `level` of the current layout.
    xkb_keysym_t sym_at(uint32_t keycode, xkb_level_index_t level) const;

    std::function<void(uint32_t time_ms, uint32_t keycode, bool pressed)> on_key;
    std::function<void()> on_modifiers;

private:
    // Re-reads the modifiers from the state; tells if they changed.
    void refresh_modifiers();

    xkb_keymap* keymap_ = nullptr;
    xkb_state* state_ = nullptr;
    Modifiers mods_;
    std::vector<uint32_t> pressed_;
    std::unordered_map<uint32_t, int> counts_;  // per keycode: keyboards holding it
    xkb_mod_index_t mod_index_[8]{};
};

} // namespace atrium::input
