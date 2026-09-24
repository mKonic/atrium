#include "keyboard_conf.hpp"

#include <fstream>
#include <sstream>

namespace atrium {

namespace {

// The next "quoted" word from `s`, which moves past it.
bool quoted(std::string_view& s, std::string& out) {
    const size_t open = s.find('"');
    if (open == std::string_view::npos)
        return false;
    const size_t close = s.find('"', open + 1);
    if (close == std::string_view::npos)
        return false;
    out = std::string(s.substr(open + 1, close - open - 1));
    s.remove_prefix(close + 1);
    return true;
}

} // namespace

XkbNames parse_x11_keyboard(std::string_view conf) {
    XkbNames n;
    while (!conf.empty()) {
        const size_t nl = conf.find('\n');
        std::string_view line = conf.substr(0, nl);
        conf.remove_prefix(nl == std::string_view::npos ? conf.size() : nl + 1);
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos || line[start] == '#')
            continue;
        line.remove_prefix(start);
        if (!line.starts_with("Option"))
            continue;
        std::string key, value;
        if (!quoted(line, key) || !quoted(line, value))
            continue;
        if (key == "XkbModel") n.model = value;
        else if (key == "XkbLayout") n.layout = value;
        else if (key == "XkbVariant") n.variant = value;
        else if (key == "XkbOptions") n.options = value;
    }
    return n;
}

const XkbNames& system_keyboard() {
    static const XkbNames names = [] {
        std::ifstream f("/etc/X11/xorg.conf.d/00-keyboard.conf");
        std::stringstream ss;
        ss << f.rdbuf();
        return parse_x11_keyboard(ss.str());
    }();
    return names;
}

} // namespace atrium
