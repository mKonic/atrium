#include "chooser_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <set>
#include <sstream>

namespace atrium::chooser {

Respond on_accept(Action action, int picked, bool all_folders, bool all_files) {
    const int k = std::min(picked, 2);
    switch (action) {
    case Action::Open:
        // 0: nothing; 1: a file answers, a folder is gone into; more: files only.
        if (k == 0)
            return Respond::Nothing;
        if (k == 1)
            return all_folders ? Respond::Enter : Respond::Answer;
        return all_files ? Respond::Answer : Respond::Nothing;
    case Action::Save:
        if (k == 1 && all_folders)
            return Respond::Enter;
        return Respond::SaveEntry;
    case Action::SelectFolder:
        // Nothing picked: the folder it's in.
        if (k == 0)
            return Respond::Answer;
        return all_folders ? Respond::Answer : Respond::Nothing;
    }
    return Respond::Nothing;
}

bool sensitive(Action action, bool is_folder) {
    return action != Action::SelectFolder || is_folder;
}

Activate on_activate(Action action, bool is_folder, bool is_sensitive) {
    if (is_sensitive && is_folder)
        return Activate::Enter;
    if (is_sensitive && (action == Action::Open || action == Action::Save))
        return Activate::Answer;
    return Activate::Nothing;
}

namespace {

std::tm local(std::int64_t secs) {
    const std::time_t t = std::time_t(secs);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

std::string format(const std::tm& tm, const char* fmt) {
    char buf[128];
    const size_t n = std::strftime(buf, sizeof buf, fmt, &tm);
    return std::string(buf, n);
}

// strftime has no "%-e": the day without its padding.
std::string day(const std::tm& tm) {
    return std::to_string(tm.tm_mday);
}

// Days between two local dates (midnight to midnight).
int days_between(std::tm from, std::tm to) {
    from.tm_hour = to.tm_hour = 12;  // DST-safe
    from.tm_min = to.tm_min = from.tm_sec = to.tm_sec = 0;
    from.tm_isdst = to.tm_isdst = -1;
    const std::time_t a = std::mktime(&from), b = std::mktime(&to);
    return int(std::lround(std::difftime(b, a) / 86400.0));
}

std::string trim_left(std::string s) {
    s.erase(0, s.find_first_not_of(' '));
    return s;
}

} // namespace

std::string time_text(std::int64_t secs, bool clock24) {
    const std::tm tm = local(secs);
    return trim_left(format(tm, clock24 ? "%H:%M" : "%l:%M %p"));
}

std::string date_text(std::int64_t secs, std::int64_t now, bool with_time, bool clock24) {
    const std::tm when = local(secs), today = local(now);
    const int days_ago = days_between(when, today);
    if (days_ago < 1)
        return with_time ? std::string() : time_text(secs, clock24);
    if (days_ago < 2)
        return "Yesterday";
    if (days_ago < 7)
        return format(when, "%a");
    if (when.tm_year == today.tm_year)
        return day(when) + format(when, " %b");
    return day(when) + format(when, " %b %Y");
}

std::string size_text(std::uint64_t bytes) {
    // g_format_size: "1 byte", "N bytes", then kB/MB/GB… with one decimal.
    if (bytes == 1)
        return "1 byte";
    if (bytes < 1000)
        return std::to_string(bytes) + " bytes";
    static const char* units[] = {"kB", "MB", "GB", "TB", "PB", "EB"};
    double v = double(bytes);
    int u = -1;
    while (v >= 1000.0 && u < 5) {
        v /= 1000.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

std::string category(std::string_view icon) {
    static const std::pair<std::string_view, std::string_view> map[] = {
        {"application-x-executable", "Program"},
        {"audio-x-generic", "Audio"},
        {"font-x-generic", "Font"},
        {"image-x-generic", "Image"},
        {"package-x-generic", "Archive"},
        {"text-html", "Markup"},
        {"text-x-generic", "Text"},
        {"text-x-generic-template", "Text"},
        {"text-x-script", "Program"},
        {"video-x-generic", "Video"},
        {"x-office-address-book", "Contacts"},
        {"x-office-calendar", "Calendar"},
        {"x-office-document", "Document"},
        {"x-office-presentation", "Presentation"},
        {"x-office-spreadsheet", "Spreadsheet"},
    };
    for (const auto& [i, name] : map)
        if (i == icon)
            return std::string(name);
    return {};
}

NameCheck check_name(std::string_view name, bool is_folder) {
    const std::string what = is_folder ? "Folder" : "File";
    const std::string a = is_folder ? "A folder" : "A file";
    if (name.empty())
        return {};
    if (name == ".")
        return {a + " cannot be called “.”", {}};
    if (name == "..")
        return {a + " cannot be called “..”", {}};
    if (name.find('/') != std::string_view::npos)
        return {what + " names cannot contain “/”", {}};
    if (std::isspace((unsigned char)name.front()))
        return {{}, what + " names should not begin with a space"};
    if (std::isspace((unsigned char)name.back()))
        return {{}, what + " names should not end with a space"};
    if (name.front() == '.')
        return {{}, what + " names starting with a “.” are hidden"};
    return {};
}

Split split(std::string_view text) {
    const size_t slash = text.rfind('/');
    if (slash == std::string_view::npos)
        return {{}, std::string(text)};
    return {std::string(text.substr(0, slash + 1)), std::string(text.substr(slash + 1))};
}

bool is_directory_shortcut(std::string_view text) {
    return text == "." || text == ".." || text == "~";
}

namespace {

bool has_uri_scheme(std::string_view s) {
    size_t i = 0;
    auto ok = [](char c) { return std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.'; };
    if (s.empty() || !ok(s[0]))
        return false;
    while (i < s.size() && ok(s[i]))
        ++i;
    return s.substr(i).starts_with("://");
}

// "a/./b/../c" → "a/c", absolute.
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream in(path);
    std::string part;
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (const std::string& p : parts)
        out += "/" + p;
    return out.empty() ? "/" : out;
}

std::string parent(const std::string& path) {
    if (path == "/")
        return {};
    const size_t slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}

} // namespace

std::string file_for_text(std::string_view text, const std::string& base, const std::string& home) {
    std::string t(text);
    if (t.starts_with("file://")) {
        if (auto p = path_for_uri(t))
            return normalize(*p);
        return {};
    }
    if (has_uri_scheme(t))
        return {};
    if (t == "~" || t.starts_with("~/"))
        return normalize(home + t.substr(1));
    if (t.starts_with("/"))
        return normalize(t);
    if (base.empty())
        return {};
    return normalize(base + "/" + t);
}

std::string directory_for_text(std::string_view text, const std::string& base, const std::string& home) {
    const std::string file = file_for_text(text, base, home);
    if (file.empty() && !text.empty())
        return {};
    if (text.empty())
        return base;
    if (text.back() == '/' || is_directory_shortcut(text))
        return file;
    return parent(file);
}

std::string common_prefix(const std::vector<std::string>& candidates, std::string_view typed) {
    std::optional<std::string> prefix;
    for (const std::string& c : candidates) {
        if (!std::string_view(c).starts_with(typed))
            continue;
        if (!prefix) {
            prefix = c;
            continue;
        }
        size_t n = 0;
        while (n < prefix->size() && n < c.size() && (*prefix)[n] == c[n])
            ++n;
        prefix->resize(n);
    }
    return prefix.value_or(std::string(typed));
}

namespace {

int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

std::string uri_for_path(std::string_view path) {
    // RFC 3986 unreserved and the path's own separators stay; the rest is %XX.
    static const char* digits = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == '!' || c == '$' ||
            c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ';' ||
            c == '=' || c == ':' || c == '@') {
            out += char(c);
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

std::optional<std::string> path_for_uri(std::string_view uri) {
    if (!uri.starts_with("file://"))
        return std::nullopt;
    uri.remove_prefix(7);
    // "file://host/path": only the local host.
    if (!uri.starts_with("/")) {
        const size_t slash = uri.find('/');
        if (slash == std::string_view::npos || uri.substr(0, slash) != "localhost")
            return std::nullopt;
        uri.remove_prefix(slash);
    }
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%') {
            if (i + 2 >= uri.size())
                return std::nullopt;
            const int hi = hex(uri[i + 1]), lo = hex(uri[i + 2]);
            if (hi < 0 || lo < 0)
                return std::nullopt;
            out += char(hi * 16 + lo);
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

std::vector<Bookmark> parse_bookmarks(std::string_view text) {
    std::vector<Bookmark> out;
    std::stringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const size_t space = line.find(' ');
        Bookmark b;
        b.uri = line.substr(0, space);
        if (space != std::string::npos)
            b.label = line.substr(space + 1);
        if (!b.uri.empty())
            out.push_back(std::move(b));
    }
    return out;
}

std::string format_bookmarks(const std::vector<Bookmark>& bookmarks) {
    std::string out;
    for (const Bookmark& b : bookmarks) {
        out += b.uri;
        if (!b.label.empty())
            out += " " + b.label;
        out += "\n";
    }
    return out;
}

namespace {

std::string xml_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

// Sets attribute `name` inside the tag text [open, close).
void set_attribute(std::string& doc, size_t open, size_t& close, std::string_view name, std::string_view value) {
    const std::string key = " " + std::string(name) + "=\"";
    const size_t at = doc.find(key, open);
    if (at != std::string::npos && at < close) {
        const size_t start = at + key.size();
        const size_t end = doc.find('"', start);
        doc.replace(start, end - start, value);
        close = doc.find('>', open);
        return;
    }
    // Before "/>" or ">".
    size_t insert = close;
    if (insert > open && doc[insert - 1] == '/')
        --insert;
    const std::string add = key + std::string(value) + "\"";
    doc.insert(insert, add);
    close += add.size();
}

} // namespace

std::string add_recent(std::string_view xbel, std::string_view uri, std::string_view mime, std::string_view app,
                       std::string_view now) {
    std::string doc(xbel);
    if (doc.find("<xbel") == std::string::npos)
        doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<xbel version=\"1.0\"\n"
              "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
              "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\"\n"
              ">\n</xbel>\n";
    const std::string href = "<bookmark href=\"" + xml_escape(uri) + "\"";
    const std::string application = "<bookmark:application name=\"" + xml_escape(app) + "\"";
    if (size_t open = doc.find(href); open != std::string::npos) {
        size_t close = doc.find('>', open);
        set_attribute(doc, open, close, "modified", now);
        set_attribute(doc, open, close, "visited", now);
        const size_t end = doc.find("</bookmark>", open);
        if (size_t a = doc.find(application, open); a != std::string::npos && a < end) {
            size_t aclose = doc.find('>', a);
            int count = 1;
            if (size_t c = doc.find(" count=\"", a); c != std::string::npos && c < aclose)
                count = std::atoi(doc.c_str() + c + 8) + 1;
            set_attribute(doc, a, aclose, "modified", now);
            set_attribute(doc, a, aclose, "count", std::to_string(count));
        } else if (size_t apps = doc.find("</bookmark:applications>", open); apps != std::string::npos && apps < end) {
            doc.insert(apps, "  " + application + " exec=\"&apos;gio open %u&apos;\" modified=\"" + std::string(now) +
                                 "\" count=\"1\"/>\n        ");
        }
        return doc;
    }
    const std::string entry =
        "  " + href + " added=\"" + std::string(now) + "\" modified=\"" + std::string(now) + "\" visited=\"" +
        std::string(now) +
        "\">\n"
        "    <info>\n"
        "      <metadata owner=\"http://freedesktop.org\">\n"
        "        <mime:mime-type type=\"" +
        xml_escape(mime) +
        "\"/>\n"
        "        <bookmark:applications>\n"
        "          " +
        application + " exec=\"&apos;gio open %u&apos;\" modified=\"" + std::string(now) +
        "\" count=\"1\"/>\n"
        "        </bookmark:applications>\n"
        "      </metadata>\n"
        "    </info>\n"
        "  </bookmark>\n";
    doc.insert(doc.rfind("</xbel>"), entry);
    return doc;
}

std::vector<std::string> recent_folders(const std::vector<std::string>& files) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& f : files) {
        const std::string p = parent(normalize(f));
        if (!p.empty() && seen.insert(p).second)
            out.push_back(p);
    }
    return out;
}

bool sorts_before(const SortKey& a, const SortKey& b, SortColumn column, bool ascending, bool folders_first) {
    if (folders_first && a.is_folder != b.is_folder)
        return a.is_folder;
    int c = 0;
    switch (column) {
    case SortColumn::Name: c = a.name_key.compare(b.name_key); break;
    case SortColumn::Size: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
    case SortColumn::Type: c = a.type.compare(b.type); break;
    case SortColumn::Modified: c = a.modified < b.modified ? -1 : a.modified > b.modified ? 1 : 0; break;
    }
    if (c == 0 && column != SortColumn::Name)
        c = a.name_key.compare(b.name_key);
    return ascending ? c < 0 : c > 0;
}

} // namespace atrium::chooser
