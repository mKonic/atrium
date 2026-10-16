#pragma once
// Which keys X11 apps hear when typed in other apps' windows, as KWin's
// "Legacy X11 App Support" decides it (xwayland.cpp XwaylandInputFilter).
// Plain C++, tested on its own.

#include <cstdint>
#include <string_view>

namespace atrium::x11_listen {

// A key that types nothing (Escape, F1, arrows, Super, media keys), not one
// an input method or a dead key uses.
bool non_character(uint32_t keysym);

// Whether `keysym`, pressed with `ctrl_alt_super` held or not, reaches X11
// apps in `mode` ("never", "modifier_keys", "shortcuts", "always").
bool hears(std::string_view mode, uint32_t keysym, bool ctrl_alt_super);

} // namespace atrium::x11_listen
