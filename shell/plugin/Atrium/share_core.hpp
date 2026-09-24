#pragma once
// The lines xdg-desktop-portal-wlr hands a dmenu-style chooser when an app
// asks to share the screen, without Qt:
//   Monitor: DP-1 Dell Inc. DELL U2720Q 1234
//   Window: Some title (identifier)
// The chooser answers with one of the lines as it came.

#include <string>
#include <string_view>
#include <vector>

namespace atrium::share {

struct Source {
    enum class Kind { Screen, Window } kind;
    std::string line;  // the whole line, the answer when chosen
    std::string name;  // the output's name, or the window's toplevel identifier
    std::string text;  // the output's description, or the window's title
};

std::vector<Source> parse(std::string_view input);

} // namespace atrium::share
