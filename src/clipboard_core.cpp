#include "clipboard_core.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

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
    const auto newest = entries_.begin() + std::ptrdiff_t(std::min(entries_.size(), kDedupe));
    auto same = std::find_if(entries_.begin(), newest, [&](const ClipboardEntry& o) {
        return o.hash == e.hash && o.size == e.size && o.mime == e.mime;
    });
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
