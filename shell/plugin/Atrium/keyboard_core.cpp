#include "keyboard_core.hpp"

namespace atrium::keyboard {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// "  code    rest" → code, rest.
bool split(std::string_view line, std::string_view& code, std::string_view& rest) {
    line = trim(line);
    const size_t space = line.find_first_of(" \t");
    if (line.empty() || space == std::string_view::npos)
        return false;
    code = line.substr(0, space);
    rest = trim(line.substr(space));
    return !rest.empty();
}

} // namespace

XkbList parse_xkb_list(std::string_view text) {
    XkbList out;
    std::string_view section;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (line.starts_with('!')) {
            section = trim(line.substr(1));
            continue;
        }
        std::string_view code, rest;
        if (!split(line, code, rest))
            continue;
        if (section == "layout") {
            out.layouts.push_back({std::string(code), std::string(rest)});
        } else if (section == "variant") {
            // "al: Albanian (Plisi)"
            const size_t colon = rest.find(':');
            if (colon == std::string_view::npos)
                continue;
            out.variants.push_back({std::string(trim(rest.substr(0, colon))), std::string(code),
                                    std::string(trim(rest.substr(colon + 1)))});
        }
    }
    return out;
}

} // namespace atrium::keyboard
