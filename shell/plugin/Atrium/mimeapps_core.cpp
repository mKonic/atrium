#include "mimeapps_core.hpp"

#include <vector>

namespace atrium::mimeapps {

namespace {

constexpr std::string_view kGroup = "[Default Applications]";

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

// The key of a "key=value" line, trimmed; "" for anything else.
std::string_view key_of(std::string_view line) {
    const size_t eq = line.find('=');
    return eq == std::string_view::npos ? std::string_view() : trim(line.substr(0, eq));
}

} // namespace

std::string default_for(std::string_view text, std::string_view mime) {
    bool in_group = false;
    for (std::string_view raw : lines(text)) {
        const std::string_view line = trim(raw);
        if (line.starts_with('[')) {
            in_group = line == kGroup;
            continue;
        }
        if (!in_group || key_of(line) != mime)
            continue;
        std::string_view value = trim(line.substr(line.find('=') + 1));
        return std::string(trim(value.substr(0, value.find(';'))));
    }
    return {};
}

std::string set_default(std::string_view text, std::string_view mime, std::string_view desktop_id) {
    const std::string entry = std::string(mime) + "=" + std::string(desktop_id) + ";";
    std::vector<std::string_view> ls = lines(text);
    std::string out;
    bool in_group = false, seen_group = false, done = false;
    // The group's last line, to add after when the key isn't there.
    auto finish_group = [&] {
        if (in_group && !done) {
            out += entry + "\n";
            done = true;
        }
    };
    for (std::string_view raw : ls) {
        const std::string_view line = trim(raw);
        if (line.starts_with('[')) {
            finish_group();
            in_group = line == kGroup;
            seen_group = seen_group || in_group;
        } else if (in_group && !done && key_of(line) == mime) {
            out += entry + "\n";
            done = true;
            continue;
        } else if (in_group && line.empty()) {
            // Keep the blank line that ends the group after the new key.
            finish_group();
        }
        out += std::string(raw) + "\n";
    }
    finish_group();
    if (!seen_group) {
        if (!out.empty() && out != "\n")
            out += "\n";
        else
            out.clear();
        out += std::string(kGroup) + "\n" + entry + "\n";
    }
    // Only as many trailing newlines as it came with (at most one added).
    if (!text.empty() && !text.ends_with('\n') && out.ends_with("\n\n"))
        out.pop_back();
    return out;
}

} // namespace atrium::mimeapps
