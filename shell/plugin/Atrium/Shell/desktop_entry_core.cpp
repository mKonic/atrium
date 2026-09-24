#include "desktop_entry_core.hpp"

#include <cctype>
#include <cstdio>

#include <algorithm>

namespace atrium::desktop_entry {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

// The spec's locale matching order: lang_COUNTRY@MODIFIER, lang_COUNTRY,
// lang@MODIFIER, lang; then the untranslated key (rank 0).
std::vector<std::string> locale_keys(std::string_view locale) {
    if (auto dot = locale.find('.'); dot != std::string_view::npos) {
        const auto at = locale.find('@', dot);
        std::string l(locale.substr(0, dot));
        if (at != std::string_view::npos)
            l += locale.substr(at);
        return locale_keys(l);
    }
    std::string lang(locale), country, modifier;
    if (auto at = lang.find('@'); at != std::string::npos) {
        modifier = lang.substr(at + 1);
        lang.resize(at);
    }
    if (auto us = lang.find('_'); us != std::string::npos) {
        country = lang.substr(us + 1);
        lang.resize(us);
    }
    std::vector<std::string> out;
    if (lang.empty() || lang == "C" || lang == "POSIX")
        return out;
    if (!country.empty() && !modifier.empty())
        out.push_back(lang + "_" + country + "@" + modifier);
    if (!country.empty())
        out.push_back(lang + "_" + country);
    if (!modifier.empty())
        out.push_back(lang + "@" + modifier);
    out.push_back(lang);
    return out;
}

bool truthy(std::string_view v) {
    return v == "true" || v == "1";
}

using Group = std::map<std::string, std::string, std::less<>>;

// A key's best value for the locale.
std::string pick(const Group& g, const std::string& key, const std::vector<std::string>& locales) {
    for (const std::string& l : locales)
        if (auto it = g.find(key + "[" + l + "]"); it != g.end())
            return it->second;
    auto it = g.find(key);
    return it == g.end() ? std::string() : it->second;
}

} // namespace

std::string unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) {
            out += value[i];
            continue;
        }
        switch (value[++i]) {
        case 's': out += ' '; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '\\': out += '\\'; break;
        default: out += '\\'; out += value[i]; break;  // \; stays for split_list
        }
    }
    return out;
}

std::vector<std::string> split_list(std::string_view value) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == ';') {
            cur += ';';
            ++i;
        } else if (value[i] == ';') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += value[i];
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::optional<Entry> parse(std::string_view text, std::string_view locale) {
    std::map<std::string, Group, std::less<>> groups;
    Group* current = nullptr;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            current = close == std::string_view::npos ? nullptr : &groups[std::string(line.substr(1, close - 1))];
            continue;
        }
        const auto eq = line.find('=');
        if (!current || eq == std::string_view::npos)
            continue;
        // The first of a duplicated key wins.
        current->try_emplace(std::string(trim(line.substr(0, eq))), std::string(trim(line.substr(eq + 1))));
    }
    auto main = groups.find("Desktop Entry");
    if (main == groups.end())
        return std::nullopt;
    const Group& g = main->second;
    const std::vector<std::string> locales = locale_keys(locale);
    auto get = [&](const char* key) { return unescape(pick(g, key, locales)); };
    auto raw = [&](const char* key) {
        auto it = g.find(key);
        return it == g.end() ? std::string() : it->second;
    };

    Entry e;
    e.type = raw("Type");
    e.name = get("Name");
    e.generic_name = get("GenericName");
    e.comment = get("Comment");
    e.icon = get("Icon");
    e.exec = unescape(raw("Exec"));
    e.try_exec = unescape(raw("TryExec"));
    e.path = unescape(raw("Path"));
    e.startup_wm_class = unescape(raw("StartupWMClass"));
    e.keywords = split_list(unescape(pick(g, "Keywords", locales)));
    e.categories = split_list(raw("Categories"));
    e.mime_types = split_list(raw("MimeType"));
    e.only_show_in = split_list(raw("OnlyShowIn"));
    e.not_show_in = split_list(raw("NotShowIn"));
    e.no_display = truthy(raw("NoDisplay"));
    e.hidden = truthy(raw("Hidden"));
    e.terminal = truthy(raw("Terminal"));
    for (const std::string& id : split_list(raw("Actions"))) {
        auto a = groups.find("Desktop Action " + id);
        if (a == groups.end())
            continue;
        Action act{.id = id,
                   .name = unescape(pick(a->second, "Name", locales)),
                   .icon = unescape(pick(a->second, "Icon", locales)),
                   .exec = unescape(pick(a->second, "Exec", {}))};
        if (!act.name.empty())
            e.actions.push_back(std::move(act));
    }
    return e;
}

std::vector<std::string> exec_argv(std::string_view exec, std::string_view name, std::string_view icon,
                                   std::string_view file) {
    std::vector<std::string> argv;
    std::string cur;
    bool in_arg = false, quoted = false;
    for (size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (quoted) {
            if (c == '"') {
                quoted = false;
            } else if (c == '\\' && i + 1 < exec.size() && std::string_view("\"`$\\").find(exec[i + 1]) != std::string_view::npos) {
                cur += exec[++i];
            } else {
                cur += c;
            }
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (in_arg)
                argv.push_back(std::move(cur));
            cur.clear();
            in_arg = false;
            continue;
        }
        if (c == '"') {
            quoted = in_arg = true;
            continue;
        }
        if (c == '%' && i + 1 < exec.size()) {
            const char code = exec[++i];
            if (code == '%') {
                cur += '%';
                in_arg = true;
            } else if (code == 'i') {
                // --icon ICON as two arguments, and only with an icon.
                if (!icon.empty() && !in_arg) {
                    argv.emplace_back("--icon");
                    argv.emplace_back(icon);
                }
            } else if (code == 'c') {
                cur += name;
                in_arg = true;
            } else if (code == 'k') {
                cur += file;
                in_arg = true;
            }
            continue;  // %f %F %u %U %d %D %n %N %v %m: nothing to hand over
        }
        cur += c;
        in_arg = true;
    }
    if (quoted)
        return {};
    if (in_arg)
        argv.push_back(std::move(cur));
    return argv;
}

bool shown_in(const Entry& e, std::string_view current_desktops) {
    std::vector<std::string> current;
    std::string cur;
    for (char c : current_desktops) {
        if (c == ':') {
            if (!cur.empty())
                current.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        current.push_back(cur);
    auto any = [&](const std::vector<std::string>& list) {
        return std::any_of(list.begin(), list.end(), [&](const std::string& d) {
            return std::find(current.begin(), current.end(), d) != current.end();
        });
    };
    if (!e.only_show_in.empty() && !any(e.only_show_in))
        return false;
    return !any(e.not_show_in);
}

std::string scope_name(std::string_view app_id, std::string_view random) {
    // systemd's escaping: letters, digits, ':', '_' and '.' (not first) as
    // they are, anything else as \xNN; so a dash in the id can't be taken
    // for the separator.
    std::string id;
    for (size_t i = 0; i < app_id.size(); ++i) {
        const unsigned char c = app_id[i];
        if (std::isalnum(c) || c == ':' || c == '_' || (c == '.' && i > 0)) {
            id += char(c);
        } else {
            char buf[5];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            id += buf;
        }
    }
    return "app-atrium-" + id + "-" + std::string(random) + ".scope";
}

} // namespace atrium::desktop_entry
