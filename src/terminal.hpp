#pragma once
// The terminal to open when the setting names none: the user's default
// (xdg-terminal-exec), else the first well-known one installed. A shell
// command line, for `sh -c`.

namespace atrium {

inline constexpr const char* kDefaultTerminal =
    "for t in xdg-terminal-exec ghostty kitty foot alacritty wezterm konsole ptyxis gnome-terminal "
    "xfce4-terminal xterm; do command -v \"$t\" >/dev/null && exec \"$t\"; done";

} // namespace atrium
