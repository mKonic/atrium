#include "clipboard_core.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace atrium {

namespace {

constexpr std::string_view kTextPreferred[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING",
                                               "TEXT"};
constexpr std::string_view kImagePreferred[] = {"image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"};

bool has(const std::vector<std::string>& v, std::string_view s) {
    return std::ranges::find(v, s) != v.end();
}

} // namespace

std::optional<std::string> clipboard_mime(const std::vector<std::string>& offered) {
    if (has(offered, "x-kde-passwordManagerHint"))
        return std::nullopt;
    for (std::string_view m : kTextPreferred)
        if (has(offered, m))
            return std::string(m);
    for (std::string_view m : kImagePreferred)
        if (has(offered, m))
            return std::string(m);
    for (const std::string& m : offered)
        if (m.starts_with("image/"))
            return m;
    return std::nullopt;
}

bool clipboard_is_text(std::string_view mime) {
    return std::ranges::find(kTextPreferred, mime) != std::end(kTextPreferred);
}

std::vector<std::string> clipboard_text_mimes() {
    return std::vector<std::string>(std::begin(kTextPreferred), std::end(kTextPreferred));
}

std::string clipboard_preview(std::string_view text, size_t max) {
    std::string out;
    size_t chars = 0;
    bool space = false;
    for (size_t i = 0; i < text.size() && chars < max;) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            space = !out.empty();
            ++i;
            continue;
        }
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 1;
        if (i + len > text.size())
            break;
        if (space) {
            out += ' ';
            ++chars;
            space = false;
            if (chars >= max)
                break;
        }
        out.append(text.substr(i, len));
        ++chars;
        i += len;
    }
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
    auto same = std::ranges::find_if(entries_, [&](const ClipboardEntry& o) {
        return o.hash == e.hash && o.size == e.size && o.mime == e.mime;
    });
    if (same != entries_.end()) {
        ClipboardEntry moved = *same;
        moved.time = e.time;
        entries_.erase(same);
        entries_.insert(entries_.begin(), moved);
        e = moved;
        *fresh = false;
        return dropped;
    }
    *fresh = true;
    entries_.insert(entries_.begin(), e);
    while (entries_.size() > limit) {
        dropped.push_back(entries_.back().id);
        entries_.pop_back();
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
    if (it != entries_.end())
        std::rotate(entries_.begin(), it, it + 1);
}

std::string ClipboardIndex::to_json() const {
    nlohmann::json list = nlohmann::json::array();
    for (const ClipboardEntry& e : entries_)
        list.push_back({{"id", e.id}, {"mime", e.mime}, {"time", e.time}, {"size", e.size}, {"hash", e.hash},
                        {"preview", e.preview}});
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
        // An id is a file name: nothing that could reach outside the store.
        if (e.id.empty() || e.id.find('/') != std::string::npos || e.id.starts_with('.') || e.mime.empty())
            continue;
        index.entries_.push_back(std::move(e));
    }
    return index;
}

} // namespace atrium
