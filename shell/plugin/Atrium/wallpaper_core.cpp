#include "wallpaper_core.hpp"

#include <vector>

namespace atrium::wallpaper {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        out.push_back(trim(text.substr(0, nl)));
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

// "key = value" (spaces optional) → value, if the line is that key.
bool value_of(std::string_view line, std::string_view key, std::string_view& value) {
    if (!line.starts_with(key))
        return false;
    std::string_view rest = trim(line.substr(key.size()));
    if (rest.empty() || rest.front() != '=')
        return false;
    value = trim(rest.substr(1));
    return true;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string clean_path(std::string_view raw, std::string_view home) {
    std::string_view s = trim(raw);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    if (s.starts_with("file://")) {
        s.remove_prefix(7);
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
                out += char(hex(s[i + 1]) * 16 + hex(s[i + 2]));
                i += 2;
            } else {
                out += s[i];
            }
        }
        return out;
    }
    if (s.starts_with("~/"))
        return std::string(home) + std::string(s.substr(1));
    return std::string(s);
}

std::string from_hyprpaper(std::string_view conf, std::string_view home) {
    std::string preload;
    bool in_block = false;
    for (std::string_view line : lines(conf)) {
        if (line.starts_with('#'))
            continue;
        std::string_view v;
        if (line.starts_with("wallpaper") && line.ends_with('{')) {
            in_block = true;
        } else if (in_block && line == "}") {
            in_block = false;
        } else if (in_block && value_of(line, "path", v)) {
            return clean_path(v, home);
        } else if (value_of(line, "wallpaper", v)) {
            const size_t comma = v.find(',');
            return clean_path(comma == std::string_view::npos ? v : v.substr(comma + 1), home);
        } else if (preload.empty() && value_of(line, "preload", v)) {
            preload = clean_path(v, home);
        }
    }
    return preload;
}

std::string from_plasma(std::string_view conf, std::string_view home) {
    bool image_group = false;
    for (std::string_view line : lines(conf)) {
        if (line.starts_with('[')) {
            image_group = line.find("[Wallpaper][org.kde.image]") != std::string_view::npos;
            continue;
        }
        std::string_view v;
        if (image_group && value_of(line, "Image", v) && !v.empty())
            return clean_path(v, home);
    }
    return {};
}

std::string from_waypaper(std::string_view conf, std::string_view home) {
    for (std::string_view line : lines(conf)) {
        std::string_view v;
        if (value_of(line, "wallpaper", v) && !v.empty())
            return clean_path(v, home);
    }
    return {};
}

} // namespace atrium::wallpaper
