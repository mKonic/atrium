#pragma once
// Which terminal to open when the setting names none: the user's default
// (xdg-terminal-exec), else the first well-known one installed, each with
// how it's told to run a command.

#include <string>

namespace atrium {

struct Terminal {
    const char* program;
    const char* run;  // what goes before a command to run in it ("" for none)
};

inline constexpr Terminal kTerminals[] = {
    {"ghostty", "-e"}, {"kitty", ""}, {"foot", ""}, {"alacritty", "-e"}, {"wezterm", "start --"},
    {"konsole", "-e"}, {"ptyxis", "--"}, {"gnome-terminal", "--"}, {"xfce4-terminal", "-x"}, {"xterm", "-e"},
};

// For `sh -c`: runs the first of xdg-terminal-exec and kTerminals installed.
inline std::string default_terminal_command() {
    std::string names = "xdg-terminal-exec";
    for (const Terminal& t : kTerminals)
        names += std::string(" ") + t.program;
    return "for t in " + names + "; do command -v \"$t\" >/dev/null && exec \"$t\"; done";
}

// How `program` runs a command: its entry in kTerminals, else "-e".
inline std::string terminal_run_args(const std::string& program) {
    for (const Terminal& t : kTerminals)
        if (program == t.program)
            return t.run;
    return "-e";
}

} // namespace atrium
