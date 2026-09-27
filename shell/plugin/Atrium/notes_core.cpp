#include "notes_core.hpp"

#include <algorithm>
#include <regex>

namespace atrium::notes {

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

} // namespace

bool unnamed(const std::string& name) {
    static const std::regex untitled("^Untitled( [0-9]+)?$");
    return std::regex_match(name, untitled);
}

std::string first_line(const std::string& text) {
    static const std::regex block(R"(^(#{1,6}\s+|>\s*|[-*+]\s+(\[[ xX]\]\s+)?|[0-9]+\.\s+))");
    static const std::regex link(R"(\[([^\]]*)\]\([^)]*\))");
    static const std::regex marks(R"(\*\*|__|~~|`|\*|_)");
    static const std::regex rule(R"(^([-*_])\1{2,}$)");
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string line = trim(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        start = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line.starts_with("```") || std::regex_match(line, rule))
            continue;
        line = std::regex_replace(line, block, "", std::regex_constants::format_first_only);
        line = std::regex_replace(line, link, "$1");
        line = trim(std::regex_replace(line, marks, ""));
        if (line.empty())
            continue;
        // 120 characters, not bytes.
        size_t chars = 0, cut = line.size();
        for (size_t i = 0; i < line.size(); ++i)
            if ((static_cast<unsigned char>(line[i]) & 0xc0) != 0x80 && ++chars == 120) {
                cut = i;
                break;
            }
        return cut < line.size() ? line.substr(0, cut) + "…" : line;
    }
    return {};
}

std::string free_name(const std::string& wanted, const std::vector<std::string>& taken, const std::string& self,
                      const std::function<std::string(const std::string&)>& fold) {
    auto clash = [&](const std::string& n) {
        const std::string file = fold(n + ".md");
        return std::ranges::any_of(taken, [&](const std::string& t) { return t != self && fold(t) == file; });
    };
    if (!clash(wanted))
        return wanted;
    for (int i = 2;; ++i) {
        const std::string n = wanted + " " + std::to_string(i);
        if (!clash(n))
            return n;
    }
}

Newline newline(const std::string& text, size_t cursor) {
    Newline out;
    cursor = std::min(cursor, text.size());
    const size_t start = cursor == 0 ? 0 : text.rfind('\n', cursor - 1);
    const size_t line_start = start == std::string::npos ? 0 : (cursor == 0 ? 0 : start + 1);
    const std::string line = text.substr(line_start, cursor - line_start);
    static const std::regex item(R"(^(\s*)([-*+]|([0-9]+)\.)(\s+)(\[[ xX]\]\s+)?)");
    std::smatch m;
    if (!std::regex_search(line, m, item))
        return out;
    // An empty item ends the list.
    if (trim(line) == trim(m[0].str())) {
        out.kind = Newline::EndList;
        out.line_start = line_start;
        return out;
    }
    const std::string marker = m[3].matched ? std::to_string(std::stoi(m[3].str()) + 1) + "." : m[2].str();
    out.kind = Newline::Continue;
    out.insert = "\n" + m[1].str() + marker + m[4].str() + (m[5].matched ? "[ ] " : "");
    return out;
}

} // namespace atrium::notes
