#include "emoji_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace atrium::emoji {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : lower(s)) {
        if (std::isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) {
            w += c;
        } else if (!w.empty()) {
            out.push_back(std::move(w));
            w.clear();
        }
    }
    if (!w.empty())
        out.push_back(std::move(w));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ')
        s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ')
        s.remove_suffix(1);
    return s;
}

} // namespace

std::vector<Emoji> parse(std::string_view text) {
    std::vector<Emoji> out;
    std::string group;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (line.starts_with("# group:")) {
            group = std::string(trim(line.substr(8)));
            continue;
        }
        if (line.empty() || line.front() == '#' || group == "Component")
            continue;
        // 1F600 ; fully-qualified # 😀 E1.0 grinning face
        const size_t semi = line.find(';'), hash = line.find('#');
        if (semi == std::string_view::npos || hash == std::string_view::npos || hash < semi)
            continue;
        if (trim(line.substr(semi + 1, hash - semi - 1)) != "fully-qualified")
            continue;
        std::string_view rest = trim(line.substr(hash + 1));
        const size_t sp1 = rest.find(' ');  // after the emoji
        if (sp1 == std::string_view::npos)
            continue;
        const size_t sp2 = rest.find(' ', sp1 + 1);  // after "E1.0"
        if (sp2 == std::string_view::npos)
            continue;
        Emoji e;
        e.text = std::string(rest.substr(0, sp1));
        e.name = std::string(rest.substr(sp2 + 1));
        e.group = group;
        if (e.name.find("skin tone") != std::string::npos)
            continue;
        e.first = char32_t(std::strtoul(std::string(trim(line.substr(0, line.find(' ')))).c_str(), nullptr, 16));
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<size_t> search(const std::vector<Emoji>& all, std::string_view query) {
    const std::vector<std::string> want = words(query);
    std::vector<size_t> out;
    for (size_t i = 0; i < all.size(); ++i) {
        const std::vector<std::string> have = words(all[i].name);
        const bool match = std::ranges::all_of(want, [&](const std::string& w) {
            return std::ranges::any_of(have, [&](const std::string& h) { return h.starts_with(w); });
        });
        if (match)
            out.push_back(i);
    }
    return out;
}

} // namespace atrium::emoji
