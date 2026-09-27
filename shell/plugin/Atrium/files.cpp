#include "files.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace atrium::files {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 31> kByType{{
    {"pdf", "application-pdf"},
    {"zip", "package-x-generic"}, {"tar", "package-x-generic"}, {"gz", "package-x-generic"},
    {"xz", "package-x-generic"}, {"zst", "package-x-generic"}, {"7z", "package-x-generic"},
    {"rar", "package-x-generic"},
    {"mp3", "audio-x-generic"}, {"flac", "audio-x-generic"}, {"ogg", "audio-x-generic"}, {"wav", "audio-x-generic"},
    {"mp4", "video-x-generic"}, {"mkv", "video-x-generic"}, {"webm", "video-x-generic"}, {"mov", "video-x-generic"},
    {"sh", "text-x-script"}, {"py", "text-x-script"}, {"fish", "text-x-script"},
    {"txt", "text-x-generic"}, {"md", "text-x-generic"}, {"json", "text-x-generic"},
    {"html", "text-html"},
    {"doc", "x-office-document"}, {"docx", "x-office-document"}, {"odt", "x-office-document"},
    {"xls", "x-office-spreadsheet"}, {"xlsx", "x-office-spreadsheet"}, {"ods", "x-office-spreadsheet"},
    {"iso", "media-optical"}, {"img", "media-optical"},
}};

} // namespace

std::string icon_for(std::string_view suffix, bool is_dir) {
    if (is_dir)
        return "folder";
    const std::string s = lower(suffix);
    for (const auto& [ext, icon] : kByType)
        if (ext == s)
            return std::string(icon);
    return "text-x-generic";
}

bool is_image(std::string_view suffix) {
    const std::string s = lower(suffix);
    for (std::string_view ext : {"png", "jpg", "jpeg", "webp", "gif", "bmp", "svg"})
        if (ext == s)
            return true;
    return false;
}

Launcher parse_launcher(std::string_view text) {
    Launcher out;
    bool in_entry = false;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            in_entry = line == "[Desktop Entry]";
            continue;
        }
        if (!in_entry)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key == "Name" && out.name.empty())
            out.name = value;
        else if (key == "Icon" && out.icon.empty())
            out.icon = value;
    }
    return out;
}

std::string free_name(const std::string& base, const std::function<bool(const std::string&)>& taken) {
    if (!taken(base))
        return base;
    for (int n = 2;; ++n)
        if (std::string name = base + " " + std::to_string(n); !taken(name))
            return name;
}

bool valid_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos &&
           name.find('\0') == std::string_view::npos;
}

std::string resolve_typed(std::string_view typed, const std::string& current, const std::string& home) {
    std::string text(trim(typed));
    if (text == "~" || text.starts_with("~/"))
        text = home + text.substr(1);
    else if (!text.starts_with("/"))
        text = current + "/" + text;
    // Take the steps: "." stays, ".." goes up (not past the root).
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t slash = text.find('/', start);
        const std::string part = text.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    std::string out;
    for (const std::string& p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

bool has_suffix(std::string_view name, const std::vector<std::string>& suffixes) {
    if (suffixes.empty())
        return true;
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return false;
    const std::string s = lower(name.substr(dot + 1));
    return std::ranges::any_of(suffixes, [&](const std::string& x) { return lower(x) == s; });
}

std::vector<std::string> filter_suffixes(std::string_view filter) {
    // Between the parentheses, if there are any.
    if (const size_t open = filter.find('('); open != std::string_view::npos) {
        const size_t close = filter.find(')', open);
        filter = filter.substr(open + 1, close == std::string_view::npos ? std::string_view::npos : close - open - 1);
    }
    std::vector<std::string> out;
    size_t start = 0;
    while (start < filter.size()) {
        size_t end = filter.find(' ', start);
        if (end == std::string_view::npos)
            end = filter.size();
        std::string_view pattern = filter.substr(start, end - start);
        if (pattern == "*" || pattern == "*.*")
            return {};
        if (pattern.starts_with("*."))
            out.emplace_back(lower(pattern.substr(2)));
        start = end + 1;
    }
    return out;
}

} // namespace atrium::files
