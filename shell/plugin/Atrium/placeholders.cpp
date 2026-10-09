#include "placeholders.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace atrium::placeholders {

namespace {

struct Token {
    size_t begin = 0, end = 0;  // [begin, end) in the text, braces included
    std::string word;           // "argument", "clipboard", ...
    std::string name;           // name="..."
    std::optional<std::string> fallback;
};

// The value of key="..." inside a placeholder's body, if given.
std::optional<std::string> attribute(const std::string& body, const std::string& key) {
    const std::string open = key + "=\"";
    size_t at = 0;
    while ((at = body.find(open, at)) != std::string::npos) {
        if (at == 0 || body[at - 1] == ' ')
            break;
        at += open.size();
    }
    if (at == std::string::npos)
        return std::nullopt;
    const size_t from = at + open.size();
    const size_t to = body.find('"', from);
    if (to == std::string::npos)
        return std::nullopt;
    return body.substr(from, to - from);
}

std::vector<Token> tokens(const std::string& text) {
    std::vector<Token> out;
    size_t i = 0;
    while ((i = text.find('{', i)) != std::string::npos) {
        const size_t close = text.find('}', i + 1);
        if (close == std::string::npos)
            break;
        const std::string body = text.substr(i + 1, close - i - 1);
        const size_t space = body.find(' ');
        const std::string word = body.substr(0, space);
        static const char* known[] = {"argument", "clipboard", "date", "time", "datetime", "uuid", "cursor"};
        if (std::ranges::find(known, word) == std::end(known) ||
            (word != "argument" && space != std::string::npos)) {
            i = i + 1;
            continue;
        }
        Token t{i, close + 1, word, {}, {}};
        if (word == "argument") {
            t.name = attribute(body, "name").value_or("Query");
            t.fallback = attribute(body, "default");
        }
        out.push_back(std::move(t));
        i = close + 1;
    }
    return out;
}

std::string encode(const std::string& s, Encoding e) {
    switch (e) {
    case Encoding::Plain: return s;
    case Encoding::Url: return url_encode(s);
    case Encoding::Shell: return shell_quote(s);
    }
    return s;
}

} // namespace

std::vector<Argument> arguments(const std::string& text) {
    std::vector<Argument> out;
    for (const Token& t : tokens(text))
        if (t.word == "argument" &&
            std::ranges::none_of(out, [&](const Argument& a) { return a.name == t.name; }))
            out.push_back({t.name, t.fallback});
    return out;
}

std::string expand(const std::string& text, const Values& v, Encoding e) {
    const std::vector<Argument> args = arguments(text);
    std::string out;
    size_t from = 0;
    for (const Token& t : tokens(text)) {
        out += text.substr(from, t.begin - from);
        from = t.end;
        if (t.word == "argument") {
            const size_t index = size_t(std::ranges::find_if(args, [&](const Argument& a) { return a.name == t.name; }) -
                                        args.begin());
            std::string value = index < v.arguments.size() ? v.arguments[index] : std::string();
            if (value.empty() && t.fallback)
                value = *t.fallback;
            out += encode(value, e);
        } else if (t.word == "clipboard") {
            out += encode(v.clipboard, e);
        } else if (t.word == "date") {
            out += encode(v.date, e);
        } else if (t.word == "time") {
            out += encode(v.time, e);
        } else if (t.word == "datetime") {
            out += encode(v.datetime, e);
        } else if (t.word == "uuid") {
            out += encode(v.uuid ? v.uuid() : std::string(), e);
        }
        // {cursor}: nothing.
    }
    out += text.substr(from);
    return out;
}

std::optional<size_t> cursor_from_end(const std::string& text, const Values& v) {
    for (const Token& t : tokens(text))
        if (t.word == "cursor") {
            const std::string after = expand(text.substr(t.end), v, Encoding::Plain);
            // Characters, not bytes: count UTF-8 lead bytes.
            return size_t(std::ranges::count_if(after, [](char c) { return (static_cast<unsigned char>(c) & 0xc0) != 0x80; }));
        }
    return std::nullopt;
}

std::string url_encode(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::optional<WebSearch> web_search(const std::string& engine, const std::string& query) {
    struct Engine {
        const char* id;
        const char* name;
        const char* address;  // the query goes after it, encoded
    };
    static constexpr Engine kEngines[] = {
        {"google", "Google", "https://www.google.com/search?q="},
        {"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q="},
        {"bing", "Bing", "https://www.bing.com/search?q="},
        {"brave", "Brave", "https://search.brave.com/search?q="},
        {"startpage", "Startpage", "https://www.startpage.com/do/search?q="},
        {"ecosia", "Ecosia", "https://www.ecosia.org/search?q="},
    };
    for (const Engine& e : kEngines)
        if (engine == e.id)
            return WebSearch{std::string("Search ") + e.name, e.address + url_encode(query)};
    return std::nullopt;
}

} // namespace atrium::placeholders
