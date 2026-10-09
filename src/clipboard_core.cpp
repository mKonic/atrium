#include "clipboard_core.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace atrium {

namespace {

constexpr std::string_view kPlainText[] = {"text/plain;charset=utf-8", "text/plain", "TEXT", "STRING",
                                           "UTF8_STRING"};
constexpr size_t kMaxRecorded = 5 * 1000 * 1000;  // cliphist's -max-store-size

bool has(const std::vector<std::string>& v, std::string_view s) {
    return std::ranges::find(v, s) != v.end();
}

} // namespace

bool clipboard_sensitive(const std::vector<std::string>& offered) {
    return has(offered, "x-kde-passwordManagerHint");
}

bool clipboard_is_text(std::string_view m) {
    // wl-clipboard's mime_type_is_text.
    const bool basic = m.starts_with("text/") || m == "TEXT" || m == "STRING" || m == "UTF8_STRING";
    const bool common = m.find("json") != std::string_view::npos || m.ends_with("script") || m.ends_with("xml") ||
                        m.ends_with("yaml") || m.ends_with("csv") || m.ends_with("ini");
    const bool keys = m.find("application/vnd.ms-publisher") != std::string_view::npos || m.ends_with("pgp-keys");
    return basic || common || keys;
}

std::vector<std::string> clipboard_history_mimes(const std::vector<std::string>& offered) {
    std::vector<std::string> out;
    if (clipboard_sensitive(offered))
        return out;
    // `wl-paste --type text`: UTF-8 plain text, plain text, any text.
    if (has(offered, "text/plain;charset=utf-8"))
        out.emplace_back("text/plain;charset=utf-8");
    else if (has(offered, "text/plain"))
        out.emplace_back("text/plain");
    else if (auto it = std::ranges::find_if(offered, [](const std::string& m) { return clipboard_is_text(m); });
             it != offered.end())
        out.push_back(*it);
    // `wl-paste --type image`: the first type starting with "image".
    if (auto it = std::ranges::find_if(offered, [](const std::string& m) { return m.starts_with("image"); });
        it != offered.end())
        out.push_back(*it);
    return out;
}

bool clipboard_worth_recording(std::string_view data) {
    if (data.size() > kMaxRecorded)
        return false;
    // bytes.TrimSpace: ASCII whitespace (and the rest of Unicode's, rare here).
    return std::ranges::any_of(data, [](char c) {
        return c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f';
    });
}

std::vector<std::string> clipboard_offer_mimes(std::string_view mime) {
    if (std::ranges::find(kPlainText, mime) != std::end(kPlainText))
        return std::vector<std::string>(std::begin(kPlainText), std::end(kPlainText));
    return {std::string(mime)};
}

std::vector<std::string> clipboard_persist_mimes(const std::vector<std::string>& offered) {
    std::vector<std::string> out;
    for (const std::string& m : offered)
        if (m != "SAVE_TARGETS" && !has(out, m))
            out.push_back(m);
    return out;
}

std::string clipboard_preview(std::string_view text, size_t max) {
    std::string out;
    size_t chars = 0;
    bool space = false, cut = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            space = !out.empty();
            ++i;
            continue;
        }
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
        if (i + len > text.size())
            break;
        if (chars + (space ? 1 : 0) >= max) {
            cut = true;
            break;
        }
        if (space) {
            out += ' ';
            ++chars;
            space = false;
        }
        out.append(text.substr(i, len));
        ++chars;
        i += len;
    }
    if (cut)
        out += "…";
    return out;
}

uint64_t clipboard_hash(std::string_view data) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h ^ data.size();
}

std::vector<std::string> ClipboardIndex::add(ClipboardEntry& e, bool* fresh, size_t limit) {
    std::vector<std::string> dropped;
    auto is_same = [&](const ClipboardEntry& o) { return o.hash == e.hash && o.size == e.size && o.mime == e.mime; };
    // Copied again: a pin keeps its place, anything else comes to the front.
    if (auto pin = std::ranges::find_if(entries_, [&](const ClipboardEntry& o) { return o.pinned && is_same(o); });
        pin != entries_.end()) {
        e = *pin;
        *fresh = false;
        return dropped;
    }
    const auto newest = entries_.begin() + std::ptrdiff_t(std::min(entries_.size(), kDedupe));
    auto same = std::find_if(entries_.begin(), newest, is_same);
    if (same != newest) {
        ClipboardEntry moved = *same;
        moved.time = e.time;
        entries_.erase(same);
        entries_.insert(entries_.begin(), moved);
        e = moved;
        *fresh = false;
        return dropped;
    }
    *fresh = true;
    e.pinned = 0;
    entries_.insert(entries_.begin(), e);
    size_t unpinned = std::ranges::count_if(entries_, [](const ClipboardEntry& o) { return !o.pinned; });
    for (auto it = entries_.end(); unpinned > limit && it != entries_.begin();) {
        --it;
        if (it->pinned)
            continue;
        dropped.push_back(it->id);
        it = entries_.erase(it);
        --unpinned;
    }
    return dropped;
}

const ClipboardEntry* ClipboardIndex::find(std::string_view id) const {
    auto it = std::ranges::find_if(entries_, [&](const ClipboardEntry& e) { return e.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

bool ClipboardIndex::remove(std::string_view id) {
    return std::erase_if(entries_, [&](const ClipboardEntry& e) { return e.id == id; }) > 0;
}

void ClipboardIndex::to_front(std::string_view id) {
    auto it = std::ranges::find_if(entries_, [&](const ClipboardEntry& e) { return e.id == id; });
    // A pin holds its place.
    if (it != entries_.end() && !it->pinned)
        std::rotate(entries_.begin(), it, it + 1);
}

bool ClipboardIndex::set_pinned(std::string_view id, bool pinned, int64_t time) {
    auto it = std::ranges::find_if(entries_, [&](const ClipboardEntry& e) { return e.id == id; });
    if (it == entries_.end())
        return false;
    if (pinned != bool(it->pinned))
        it->pinned = pinned ? std::max<int64_t>(time, 1) : 0;
    return true;
}

std::vector<std::string> ClipboardIndex::clear_unpinned() {
    std::vector<std::string> gone;
    std::erase_if(entries_, [&](const ClipboardEntry& e) {
        if (e.pinned)
            return false;
        gone.push_back(e.id);
        return true;
    });
    return gone;
}

std::vector<const ClipboardEntry*> ClipboardIndex::listed() const {
    std::vector<const ClipboardEntry*> pins, rest;
    for (const ClipboardEntry& e : entries_)
        (e.pinned ? pins : rest).push_back(&e);
    std::ranges::stable_sort(pins, {}, [](const ClipboardEntry* e) { return e->pinned; });
    pins.insert(pins.end(), rest.begin(), rest.end());
    return pins;
}

std::string ClipboardIndex::to_json() const {
    nlohmann::json list = nlohmann::json::array();
    for (const ClipboardEntry& e : entries_)
        list.push_back({{"id", e.id}, {"mime", e.mime}, {"time", e.time}, {"size", e.size}, {"hash", e.hash},
                        {"preview", e.preview}, {"pinned", e.pinned}});
    return list.dump();
}

ClipboardIndex ClipboardIndex::from_json(std::string_view text) {
    ClipboardIndex index;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array())
        return index;
    for (const nlohmann::json& o : j) {
        if (!o.is_object() || !o.contains("id") || !o["id"].is_string())
            continue;
        // A hand-edited or damaged field reads as missing.
        auto string = [&](const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : ""; };
        auto number = [&]<class T>(const char* k, T) {
            return o.contains(k) && o[k].is_number_integer() ? o[k].get<T>() : T(0);
        };
        ClipboardEntry e;
        e.id = o["id"];
        e.mime = string("mime");
        e.time = number("time", int64_t(0));
        e.size = number("size", uint64_t(0));
        e.hash = number("hash", uint64_t(0));
        e.preview = string("preview");
        e.pinned = number("pinned", int64_t(0));
        // An id is a file name: nothing that could reach outside the store.
        if (e.id.empty() || e.id.find('/') != std::string::npos || e.id.starts_with('.') || e.mime.empty())
            continue;
        index.entries_.push_back(std::move(e));
    }
    return index;
}

// --- what an entry is (Tinycast's ClipboardFilter and ColorValue) -----------------------

namespace {

bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && is_space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && is_space(s.back()))
        s.remove_suffix(1);
    return s;
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

int hex_value(char c) {
    if (is_digit(c))
        return c - '0';
    c = char(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = char(c | 0x20);
    return out;
}

double clamp01(double v) {
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

ClipboardColor rgba(double r, double g, double b, double a) {
    return {clamp01(r), clamp01(g), clamp01(b), clamp01(a)};
}

ClipboardColor from_hsl(double hue, double saturation, double lightness, double alpha) {
    saturation = std::clamp(saturation, 0.0, 1.0);
    lightness = std::clamp(lightness, 0.0, 1.0);
    const double chroma = (1 - std::abs(2 * lightness - 1)) * saturation;
    const double sector = hue / 60;
    const double second = chroma * (1 - std::abs(std::fmod(sector, 2) - 1));
    const double base = lightness - chroma / 2;
    double r = 0, g = 0, b = 0;
    if (sector < 1)
        r = chroma, g = second;
    else if (sector < 2)
        r = second, g = chroma;
    else if (sector < 3)
        g = chroma, b = second;
    else if (sector < 4)
        g = second, b = chroma;
    else if (sector < 5)
        r = second, b = chroma;
    else
        r = chroma, b = second;
    return rgba(r + base, g + base, b + base, alpha);
}

double gamma_encode(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

ClipboardColor from_oklch(double lightness, double chroma, double hue, double alpha) {
    const double radians = hue * M_PI / 180;
    const double a = chroma * std::cos(radians), b = chroma * std::sin(radians);
    auto cubed = [](double v) { return v * v * v; };
    const double l = cubed(lightness + 0.3963377774 * a + 0.2158037573 * b);
    const double m = cubed(lightness - 0.1055613458 * a - 0.0638541728 * b);
    const double s = cubed(lightness - 0.0894841775 * a - 1.2914855480 * b);
    return rgba(gamma_encode(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
                gamma_encode(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
                gamma_encode(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s), alpha);
}

std::optional<ClipboardColor> parse_hex(std::string_view digits) {
    std::vector<double> channels;
    for (char c : digits)
        if (hex_value(c) < 0)
            return std::nullopt;
    if (digits.size() == 3 || digits.size() == 4) {
        // Shorthand doubles each digit: #0f0 is #00ff00.
        for (char c : digits)
            channels.push_back(hex_value(c) * 17 / 255.0);
    } else if (digits.size() == 6 || digits.size() == 8) {
        for (size_t i = 0; i < digits.size(); i += 2)
            channels.push_back((hex_value(digits[i]) * 16 + hex_value(digits[i + 1])) / 255.0);
    } else {
        return std::nullopt;
    }
    return rgba(channels[0], channels[1], channels[2], channels.size() == 4 ? channels[3] : 1);
}

// One channel as a share of `scale`; a trailing % is always a share of the whole.
std::optional<double> component(std::string_view text, double scale) {
    if (text.empty())
        return std::nullopt;
    for (char c : text)
        if (!is_digit(c) && c != '.' && c != '-' && c != '%')
            return std::nullopt;
    const bool percent = text.back() == '%';
    if (percent)
        text.remove_suffix(1);
    double v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || end != text.data() + text.size() || !std::isfinite(v))
        return std::nullopt;
    return percent ? v / 100 : v / scale;
}

std::optional<double> percentage(std::string_view text) {
    return !text.empty() && text.back() == '%' ? component(text, 1) : std::nullopt;
}

// A hue in degrees, into 0..360 (so -30deg and 330 agree).
std::optional<double> angle(std::string_view text) {
    std::string t = lower(text);
    if (t.ends_with("deg"))
        t.resize(t.size() - 3);
    double v = 0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (t.empty() || ec != std::errc() || end != t.data() + t.size() || !std::isfinite(v))
        return std::nullopt;
    v = std::fmod(v, 360);
    return v < 0 ? v + 360 : v;
}

std::vector<std::string_view> split(std::string_view s, char by) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == by) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i]))
            ++i;
        const size_t start = i;
        while (i < s.size() && !is_space(s[i]))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

// Comma-separated, or space-separated with alpha behind a slash; never mixed.
std::optional<std::vector<std::string_view>> arguments(std::string_view body) {
    std::vector<std::string_view> parts;
    if (body.find(',') != std::string_view::npos) {
        if (body.find('/') != std::string_view::npos)
            return std::nullopt;
        for (std::string_view p : split(body, ','))
            parts.push_back(trimmed(p));
    } else {
        const auto sides = split(body, '/');
        if (sides.size() == 1 && words(sides[0]).size() == 3) {
            parts = words(sides[0]);
        } else if (sides.size() == 2 && words(sides[0]).size() == 3 && words(sides[1]).size() == 1) {
            parts = words(sides[0]);
            parts.push_back(words(sides[1])[0]);
        } else {
            return std::nullopt;
        }
    }
    for (std::string_view p : parts)
        if (p.empty())
            return std::nullopt;
    return parts;
}

std::optional<ClipboardColor> parse_functional(std::string_view token) {
    const size_t open = token.find('(');
    if (open == std::string_view::npos || token.back() != ')')
        return std::nullopt;
    const std::string function = lower(token.substr(0, open));
    const auto parts = arguments(token.substr(open + 1, token.size() - open - 2));
    if (!parts || (parts->size() != 3 && parts->size() != 4))
        return std::nullopt;
    const auto& p = *parts;
    const std::optional<double> alpha = p.size() == 4 ? component(p[3], 1) : std::optional<double>(1);
    if (!alpha)
        return std::nullopt;
    if (function == "rgb" || function == "rgba") {
        const auto r = component(p[0], 255), g = component(p[1], 255), b = component(p[2], 255);
        if (!r || !g || !b)
            return std::nullopt;
        return rgba(*r, *g, *b, *alpha);
    }
    if (function == "hsl" || function == "hsla") {
        // CSS writes both as percentages: a bare 100 would read as 1.0.
        const auto h = angle(p[0]);
        const auto sat = percentage(p[1]), light = percentage(p[2]);
        if (!h || !sat || !light)
            return std::nullopt;
        return from_hsl(*h, *sat, *light, *alpha);
    }
    if (function == "oklch") {
        // A percentage chroma is a share of 0.4, the bound CSS gives it.
        const auto l = component(p[0], 1), c = component(p[1], 1);
        const auto h = angle(p[2]);
        if (!l || !c || !h)
            return std::nullopt;
        return from_oklch(*l, p[1].back() == '%' ? *c * 0.4 : *c, *h, *alpha);
    }
    return std::nullopt;
}

// A host's last label, when every label is one (letters, digits, hyphens).
std::optional<std::string_view> top_level_label(std::string_view host) {
    const auto labels = split(host, '.');
    if (labels.size() < 2)
        return std::nullopt;
    for (std::string_view l : labels) {
        if (l.empty())
            return std::nullopt;
        for (char c : l)
            if (!is_alpha(c) && !is_digit(c) && c != '-')
                return std::nullopt;
    }
    const std::string_view tld = labels.back();
    if (tld.size() < 2 || !std::ranges::all_of(tld, is_alpha))
        return std::nullopt;
    return tld;
}

bool is_address(std::string_view token) {
    const auto parts = split(token, '@');
    return parts.size() == 2 && !parts[0].empty() && top_level_label(parts[1]);
}

// A scheme-less link needs a top-level domain people copy, or report.pdf reads as one.
bool is_bare_domain(std::string_view token) {
    const std::string_view host = token.substr(0, token.find_first_of("/?#:"));
    if (lower(host.substr(0, 4)) == "www.")
        return true;
    // Hosts are lower case, which keeps Safari.app out.
    if (std::ranges::any_of(host, [](char c) { return c >= 'A' && c <= 'Z'; }))
        return false;
    const auto tld = top_level_label(host);
    static constexpr std::array<std::string_view, 50> kCommon = {
        "com", "org", "net", "edu", "gov", "io", "co", "ai", "app", "dev", "me", "info", "biz",
        "xyz", "tv", "ly", "gg", "to", "uk", "us", "eu", "de", "fr", "es", "it", "nl", "se", "no",
        "fi", "dk", "ch", "at", "be", "ie", "cz", "ru", "ua", "tr", "cn", "jp", "kr", "hk", "sg",
        "au", "nz", "ca", "mx", "br", "ar", "za"};
    return tld && std::ranges::find(kCommon, *tld) != kCommon.end();
}

// mailto: is an address; any other scheme:// is a link.
std::optional<ClipboardKind> scheme_kind(std::string_view token) {
    if (lower(token.substr(0, 7)) == "mailto:")
        return ClipboardKind::Email;
    const size_t sep = token.find("://");
    if (sep == std::string_view::npos || sep == 0)
        return std::nullopt;
    for (char c : token.substr(0, sep))
        if (!is_alpha(c) && c != '+' && c != '-' && c != '.')
            return std::nullopt;
    return ClipboardKind::Link;
}

// Every line a file:// URI, as Dolphin and Nautilus copy files.
bool is_file_list(std::string_view text) {
    bool any = false;
    for (std::string_view line : split(text, '\n')) {
        line = trimmed(line);
        if (line.empty())
            continue;
        if (!line.starts_with("file://"))
            return false;
        any = true;
    }
    return any;
}

} // namespace

const char* clipboard_kind_name(ClipboardKind kind) {
    switch (kind) {
    case ClipboardKind::Text: return "text";
    case ClipboardKind::Image: return "image";
    case ClipboardKind::File: return "file";
    case ClipboardKind::Color: return "color";
    case ClipboardKind::Link: return "link";
    case ClipboardKind::Email: return "email";
    }
    return "text";
}

std::optional<ClipboardColor> clipboard_color(std::string_view text) {
    if (text.size() > 2048)
        return std::nullopt;
    const std::string_view token = trimmed(text);
    // A colour is one short token: nothing past the longest hsla() form.
    if (token.empty() || token.size() > 64)
        return std::nullopt;
    if (token.front() == '#')
        return parse_hex(token.substr(1));
    if (!is_alpha(token.front()))
        return std::nullopt;
    return parse_functional(token);
}

std::string clipboard_color_hex(const ClipboardColor& c) {
    auto byte = [](double v) { return int(std::lround(v * 255)); };
    char out[10];
    if (byte(c.alpha) < 255)
        std::snprintf(out, sizeof out, "#%02x%02x%02x%02x", byte(c.red), byte(c.green), byte(c.blue), byte(c.alpha));
    else
        std::snprintf(out, sizeof out, "#%02x%02x%02x", byte(c.red), byte(c.green), byte(c.blue));
    return out;
}

ClipboardKind clipboard_kind(std::string_view mime, std::string_view data) {
    if (mime.starts_with("image/"))
        return ClipboardKind::Image;
    if (mime == "text/uri-list" || (data.size() <= 64 * 1024 && is_file_list(data)))
        return ClipboardKind::File;
    // A link or an address is one short token: anything longer is prose.
    if (data.size() > 2048)
        return ClipboardKind::Text;
    const std::string_view token = trimmed(data);
    if (token.empty())
        return ClipboardKind::Text;
    // Before the whitespace test: rgb(255, 87, 51) is one value with spaces.
    if (clipboard_color(token))
        return ClipboardKind::Color;
    if (std::ranges::any_of(token, is_space))
        return ClipboardKind::Text;
    if (const auto k = scheme_kind(token))
        return *k;
    if (is_address(token))
        return ClipboardKind::Email;
    return is_bare_domain(token) ? ClipboardKind::Link : ClipboardKind::Text;
}

} // namespace atrium
