#include "search.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace atrium::search {

namespace {

bool separator(char c) {
    return c == ' ' || c == '-' || c == '_' || c == '.' || c == ':' || c == '/';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ')
        s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ')
        s.remove_suffix(1);
    return s;
}

} // namespace

int score(std::string_view query, std::string_view text) {
    const std::string_view q = trim(query);
    if (q.empty() || text.empty())
        return 0;
    if (text == q)
        return 1000;
    if (text.starts_with(q))
        return 800 - int(std::min<size_t>(text.size() - q.size(), 100));

    // Word starts, and the initials they spell.
    std::string initials;
    bool word_start = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (separator(text[i]))
            continue;
        if (i == 0 || separator(text[i - 1])) {
            if (text.substr(i).starts_with(q))
                word_start = true;
            initials += text[i];
        }
    }
    if (word_start)
        return 600;
    if (q.size() > 1 && std::string_view(initials).starts_with(q))
        return 500;

    if (const size_t at = text.find(q); at != std::string_view::npos)
        return 400 - int(std::min<size_t>(at, 100));

    // Every letter, in order, close together: letters scattered across a
    // long description are chance, not a match.
    size_t from = 0;
    long last = -1, gaps = 0;
    for (char c : q) {
        if (c == ' ')
            continue;
        const size_t found = text.find(c, from);
        if (found == std::string_view::npos)
            return 0;
        if (last >= 0)
            gaps += long(found) - last - 1;
        last = long(found);
        from = found + 1;
    }
    const int s = 200 - int(gaps) * 8;
    return s >= 120 ? s : 0;
}

namespace {

// Recursive descent over the expression grammar; throws on anything else.
class Parser {
public:
    explicit Parser(std::string s) : s_(std::move(s)) {}

    double parse() {
        const double v = sum();
        if (i_ != s_.size())
            throw 0;
        return v;
    }

private:
    bool eat(char c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    double sum() {
        double v = product();
        for (;;) {
            if (eat('+'))
                v += product();
            else if (eat('-'))
                v -= product();
            else
                return v;
        }
    }

    double product() {
        double v = power();
        for (;;) {
            if (eat('*'))
                v *= power();
            else if (eat('/'))
                v /= power();
            else if (eat('%'))
                v = std::fmod(v, power());
            else
                return v;
        }
    }

    double power() {
        const double base = unary();
        return eat('^') ? std::pow(base, power()) : base;  // right-associative
    }

    double unary() {
        if (eat('-'))
            return -unary();
        if (eat('+'))
            return unary();
        return primary();
    }

    double primary() {
        if (eat('(')) {
            const double v = sum();
            if (!eat(')'))
                throw 0;
            return v;
        }
        if (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_]))) {
            std::string name;
            while (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_])))
                name += char(std::tolower(static_cast<unsigned char>(s_[i_++])));
            if (name == "pi")
                return M_PI;
            if (name == "e")
                return M_E;
            if (!eat('('))
                throw 0;
            const double v = sum();
            if (!eat(')'))
                throw 0;
            if (name == "sqrt") return std::sqrt(v);
            if (name == "sin") return std::sin(v);
            if (name == "cos") return std::cos(v);
            if (name == "tan") return std::tan(v);
            if (name == "log") return std::log10(v);
            if (name == "ln") return std::log(v);
            if (name == "abs") return std::fabs(v);
            throw 0;
        }
        return number();
    }

    double number() {
        const char* start = s_.c_str() + i_;
        char* end = nullptr;
        const double v = std::strtod(start, &end);
        if (end == start || std::isalpha(static_cast<unsigned char>(*start)))
            throw 0;
        i_ += size_t(end - start);
        return v;
    }

    std::string s_;
    size_t i_ = 0;
};

} // namespace

std::optional<double> calculate(std::string_view text) {
    std::string src;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t')
            continue;
        src += c == ',' ? '.' : c;
    }
    // Multiplication and division signs as people type them (UTF-8).
    for (auto [from, to] : {std::pair<std::string, char>{"\xc3\x97", '*'}, {"\xc3\xb7", '/'}}) {
        for (size_t at; (at = src.find(from)) != std::string::npos;)
            src.replace(at, from.size(), 1, to);
    }
    if (src.empty())
        return std::nullopt;
    // A calculation has an operator (past a leading sign) or a function/constant.
    const std::string_view body = std::string_view(src).substr(src[0] == '-' ? 1 : 0);
    const bool has_op = body.find_first_of("+-*/%^(") != std::string_view::npos;
    const bool has_name = std::ranges::any_of(src, [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
    if (!has_op && !has_name)
        return std::nullopt;
    // strtod would read "inf", "nan" and hex; none of those are sums here.
    if (src.find("inf") != std::string::npos || src.find("nan") != std::string::npos ||
        src.find("0x") != std::string::npos)
        return std::nullopt;
    try {
        const double v = Parser(src).parse();
        if (!std::isfinite(v))
            return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::string format_number(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", v);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

} // namespace atrium::search
