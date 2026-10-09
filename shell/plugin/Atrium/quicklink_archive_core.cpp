#include "quicklink_archive_core.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace atrium::quicklinks {

namespace {

std::string trimmed(std::string_view s) {
    const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (!s.empty() && space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && space(s.back()))
        s.remove_suffix(1);
    return std::string(s);
}

std::string folded(std::string_view s) {
    std::string out = trimmed(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

} // namespace

std::string encode(const std::vector<Quicklink>& quicklinks) {
    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const Quicklink& q : quicklinks) {
        nlohmann::ordered_json o = {{"name", q.name}, {"link", q.link}};
        if (!q.app.empty())
            o["app"] = q.app;
        if (!q.icon.empty())
            o["icon"] = q.icon;
        o["root"] = q.root;
        list.push_back(std::move(o));
    }
    return nlohmann::ordered_json{{"version", kVersion}, {"quicklinks", list}}.dump(2) + "\n";
}

std::variant<std::vector<Quicklink>, std::string> decode(std::string_view text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    const nlohmann::json* list = nullptr;
    if (j.is_object() && j.contains("quicklinks") && j["quicklinks"].is_array())
        list = &j["quicklinks"];
    else if (j.is_array())
        list = &j;
    if (!list)
        return std::string("This file isn't a quicklinks export.");
    std::vector<Quicklink> out;
    for (const nlohmann::json& o : *list) {
        if (!o.is_object())
            continue;
        auto text_of = [&](const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : ""; };
        Quicklink q{text_of("name"), text_of("link"), text_of("app"), text_of("icon"), true};
        // atrium's own records say url.
        if (q.link.empty())
            q.link = text_of("url");
        if (o.contains("root") && o["root"].is_boolean())
            q.root = o["root"].get<bool>();
        out.push_back(std::move(q));
    }
    if (out.empty())
        return std::string("This file has no quicklinks.");
    return out;
}

Merge merge(const std::vector<Quicklink>& incoming, const std::vector<Quicklink>& existing) {
    std::set<std::string> names, links;
    for (const Quicklink& q : existing) {
        names.insert(folded(q.name));
        links.insert(trimmed(q.link));
    }
    Merge out;
    for (const Quicklink& q : incoming) {
        const std::string name = folded(q.name), link = trimmed(q.link);
        if (name.empty() || link.empty() || names.contains(name) || links.contains(link)) {
            ++out.skipped;
            continue;
        }
        names.insert(name);
        links.insert(link);
        Quicklink kept = q;
        kept.name = trimmed(q.name);
        kept.link = link;
        out.additions.push_back(std::move(kept));
    }
    return out;
}

} // namespace atrium::quicklinks
