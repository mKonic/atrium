#include "portal_core.hpp"

namespace atrium::portal {

namespace {

// Percent-encoded, all but the unreserved characters and `keep`.
std::string encode(std::string_view s, std::string_view keep = {}) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~' || keep.find(ch) != std::string_view::npos) {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string addresses(const std::vector<std::string>& list) {
    std::string out;
    for (const std::string& a : list) {
        if (a.empty())
            continue;
        if (!out.empty())
            out += ',';
        out += encode(a, "@");
    }
    return out;
}

} // namespace

std::string mailto(const Email& e) {
    std::string url = "mailto:" + addresses(e.to);
    char sep = '?';
    auto field = [&](std::string_view key, const std::string& value) {
        url += sep;
        url += key;
        url += '=';
        url += value;
        sep = '&';
    };
    if (const std::string cc = addresses(e.cc); !cc.empty())
        field("cc", cc);
    if (const std::string bcc = addresses(e.bcc); !bcc.empty())
        field("bcc", bcc);
    if (e.has_subject)
        field("subject", encode(e.subject));
    if (e.has_body)
        field("body", encode(e.body));
    return url;
}

uint8_t urgency(std::string_view priority) {
    if (priority == "low")
        return 0;
    if (priority == "urgent")
        return 2;
    return 1;
}

} // namespace atrium::portal
