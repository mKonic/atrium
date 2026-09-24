#include "ini_core.hpp"

#include <vector>

namespace atrium::ini {

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
        out.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

std::string_view key_of(std::string_view line) {
    const size_t eq = line.find('=');
    return eq == std::string_view::npos ? std::string_view() : trim(line.substr(0, eq));
}

bool is_group(std::string_view line, std::string_view group) {
    return line.size() == group.size() + 2 && line.front() == '[' && line.back() == ']' &&
           line.substr(1, group.size()) == group;
}

} // namespace

std::optional<std::string> get(std::string_view text, std::string_view group, std::string_view key) {
    bool in_group = false;
    for (std::string_view raw : lines(text)) {
        const std::string_view line = trim(raw);
        if (line.starts_with('['))
            in_group = is_group(line, group);
        else if (in_group && !line.starts_with('#') && key_of(line) == key)
            return std::string(trim(line.substr(line.find('=') + 1)));
    }
    return std::nullopt;
}

std::string set(std::string_view text, std::string_view group, std::string_view key,
                std::optional<std::string_view> value) {
    const std::string entry = value ? std::string(key) + "=" + std::string(*value) : std::string();
    std::string out;
    bool in_group = false, seen_group = false, done = false;
    auto finish_group = [&] {
        if (in_group && !done && value) {
            out += entry + "\n";
            done = true;
        }
    };
    for (std::string_view raw : lines(text)) {
        const std::string_view line = trim(raw);
        if (line.starts_with('[')) {
            finish_group();
            in_group = is_group(line, group);
            seen_group = seen_group || in_group;
        } else if (in_group && !done && !line.starts_with('#') && key_of(line) == key) {
            if (value)
                out += entry + "\n";
            done = true;
            continue;
        } else if (in_group && line.empty()) {
            // The new key goes before the blank line that ends the group.
            finish_group();
        }
        out += std::string(raw) + "\n";
    }
    finish_group();
    if (!seen_group && value) {
        if (!out.empty())
            out += "\n";
        out += "[" + std::string(group) + "]\n" + entry + "\n";
    }
    if (!text.empty() && !text.ends_with('\n') && out.ends_with("\n\n"))
        out.pop_back();
    return out;
}

} // namespace atrium::ini
