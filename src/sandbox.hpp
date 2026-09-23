#pragma once
#include <string_view>

namespace atrium {

// Protocols a sandboxed app (Flatpak, over security-context) never sees:
// the ones that read the screen, the clipboard or other windows, fake
// input, or change the session and its displays.
bool privileged_protocol(std::string_view interface);

} // namespace atrium
