#pragma once
// Which terminal to open when the setting names none: the user's default
// (xdg-terminal-exec), else the first well-known one installed, each with
// how it's told to run a command.

#include <string>

namespace atrium {

struct Terminal {
    const char* program;
    const char* run;  // what goes before a command to run in it ("" for none)
    // What goes before a folder to start in it ("" when it only inherits
    // ours). Ghostty hands a new window to one already running, which
    // opens it in that one's folder, unless told not to.
    const char* dir;
};

inline constexpr Terminal kTerminals[] = {
    {"ghostty", "-e", "--gtk-single-instance=false --working-directory="},
    {"kitty", "", "--directory="},
    {"foot", "", "--working-directory="},
    {"alacritty", "-e", "--working-directory="},
    {"wezterm", "start --", "start --cwd "},
    {"konsole", "-e", "--workdir "},
    {"ptyxis", "--", "--working-directory="},
    {"gnome-terminal", "--", "--working-directory="},
    {"xfce4-terminal", "-x", "--working-directory="},
    {"xterm", "-e", ""},
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

// For `sh -c`: `command` (already quoted for sh) in `configured` (the
// terminal setting, a command line) or, when that's empty, the first of
// xdg-terminal-exec and kTerminals installed.
inline std::string terminal_running(const std::string& configured, const std::string& command) {
    if (!configured.empty()) {
        const std::string program = configured.substr(0, configured.find(' '));
        const std::string run = terminal_run_args(program.substr(program.rfind('/') + 1));
        return "exec " + configured + (run.empty() ? "" : " " + run) + " " + command;
    }
    std::string out = "if command -v xdg-terminal-exec >/dev/null; then exec xdg-terminal-exec " + command + "; fi";
    for (const Terminal& t : kTerminals)
        out += std::string("; if command -v ") + t.program + " >/dev/null; then exec " + t.program +
               (*t.run ? std::string(" ") + t.run : "") + " " + command + "; fi";
    return out;
}

// For `sh -c`: the terminal opening in `dir` (already quoted for sh), as
// `configured` (the terminal setting) or the first installed. It starts in
// `dir` too, for one that only inherits it.
inline std::string terminal_in(const std::string& configured, const std::string& dir) {
    auto with_dir = [&](const std::string& program) {
        for (const Terminal& t : kTerminals)
            if (program == t.program && *t.dir)
                return std::string(" ") + t.dir + dir;
        return std::string();
    };
    std::string out = "cd " + dir + " || exit";
    if (!configured.empty()) {
        const std::string program = configured.substr(0, configured.find(' '));
        return out + "; exec " + configured + with_dir(program.substr(program.rfind('/') + 1));
    }
    out += "; if command -v xdg-terminal-exec >/dev/null; then exec xdg-terminal-exec; fi";
    for (const Terminal& t : kTerminals)
        out += std::string("; if command -v ") + t.program + " >/dev/null; then exec " + t.program +
               with_dir(t.program) + "; fi";
    return out;
}

} // namespace atrium
