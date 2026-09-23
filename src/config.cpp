#include "config.hpp"
#include "settings.hpp"

namespace atrium {

const Keybind* find_keybind(const std::vector<Keybind>& binds, uint32_t mods, xkb_keysym_t sym) {
    for (const Keybind& b : binds)
        if (clean_mods(mods) == clean_mods(b.mods) && sym == b.sym)
            return &b;
    return nullptr;
}

Config Config::defaults(bool nested) {
    Config c;
    c.mod = nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
    c.keybinds = resolve_keybinds(default_keybinds(), c.mod);
    return c;
}

} // namespace atrium
