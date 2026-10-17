#include "file_search_core.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace atrium::file_search {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string_view> components(std::string_view path) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!part.empty())
            out.push_back(part);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return out;
}

// "%2F" and friends in a file:// URI.
std::string percent_decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// "2026-09-27T16:10:02.123456Z" → unix seconds (UTC).
long long parse_time(std::string_view iso) {
    std::tm tm{};
    std::istringstream in{std::string(iso)};
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail())
        return 0;
    return static_cast<long long>(timegm(&tm));
}

std::string attribute(std::string_view tag, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const size_t at = tag.find(key);
    if (at == std::string_view::npos)
        return {};
    const size_t from = at + key.size();
    const size_t to = tag.find('"', from);
    return to == std::string_view::npos ? std::string() : std::string(tag.substr(from, to - from));
}

// XML's own escapes in an attribute.
std::string unescape(std::string s) {
    const std::pair<const char*, const char*> table[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                                                          {"&quot;", "\""}, {"&apos;", "'"}};
    for (const auto& [from, to] : table) {
        size_t at = 0;
        while ((at = s.find(from, at)) != std::string::npos) {
            s.replace(at, std::string_view(from).size(), to);
            at += std::string_view(to).size();
        }
    }
    return s;
}

} // namespace

const std::vector<std::string>& IgnoreList::defaults() {
    static const std::vector<std::string> d = {"node_modules", "__pycache__", "*.pyc", "*.o", "*.tmp", "*~"};
    return d;
}

IgnoreList::IgnoreList(const std::vector<std::string>& patterns) {
    for (const std::string& raw : patterns) {
        const std::string p = lower(raw);
        if (p.empty())
            continue;
        if (p.find('/') != std::string::npos)
            path_globs_.push_back(p);
        else if (p.find_first_of("*?[") != std::string::npos)
            component_globs_.push_back(p);
        else
            names_.push_back(p);
    }
}

bool IgnoreList::ignores(std::string_view path) const {
    const std::string l = lower(path);
    for (std::string_view c : components(l)) {
        const std::string part(c);
        if (std::ranges::find(names_, part) != names_.end())
            return true;
        for (const std::string& g : component_globs_)
            if (fnmatch(g.c_str(), part.c_str(), 0) == 0)
                return true;
    }
    // Without FNM_PATHNAME, so * spans / and **/x/** reads as written.
    for (const std::string& g : path_globs_)
        if (fnmatch(g.c_str(), l.c_str(), 0) == 0)
            return true;
    return false;
}

bool admitted(std::string_view path, const std::vector<std::string>& roots, const IgnoreList& ignore) {
    for (const std::string& root : roots) {
        const bool under = root == "/" ? path.starts_with('/')
                                       : path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/';
        if (!under)
            continue;
        const std::string_view rest = path.substr(root == "/" ? 1 : root.size() + 1);
        for (std::string_view c : components(rest))
            if (c.starts_with('.'))
                return false;
        return !ignore.ignores(rest);
    }
    return false;
}

std::vector<std::string> terms(std::string_view query) {
    std::vector<std::string> out;
    std::istringstream in{lower(query)};
    std::string t;
    while (in >> t)
        out.push_back(t);
    return out;
}

bool name_matches(std::string_view name, const std::vector<std::string>& terms) {
    if (terms.empty())
        return false;
    const std::string l = lower(name);
    return std::ranges::all_of(terms, [&](const std::string& t) { return l.find(t) != std::string::npos; });
}

bool filter_accepts(std::string_view filter, std::string_view mime) {
    if (filter.empty() || filter == "all")
        return true;
    const bool folder = mime == "inode/directory";
    if (filter == "folders")
        return folder;
    if (folder)
        return false;
    if (filter == "images")
        return mime.starts_with("image/");
    if (filter == "audio")
        return mime.starts_with("audio/");
    if (filter == "videos")
        return mime.starts_with("video/");
    if (filter == "archives") {
        static const char* archives[] = {"application/zip", "application/x-tar", "application/gzip",
                                         "application/x-7z-compressed", "application/x-rar",
                                         "application/vnd.rar", "application/x-xz", "application/zstd",
                                         "application/x-bzip2", "application/x-compressed-tar",
                                         "application/x-xz-compressed-tar", "application/x-zstd-compressed-tar",
                                         "application/x-bzip2-compressed-tar", "application/x-iso9660-image"};
        return std::ranges::find(archives, mime) != std::end(archives);
    }
    if (filter == "documents")
        return mime.starts_with("text/") || mime == "application/pdf" ||
               mime.find("opendocument") != std::string_view::npos ||
               mime.find("officedocument") != std::string_view::npos || mime == "application/msword" ||
               mime == "application/vnd.ms-excel" || mime == "application/vnd.ms-powerpoint" ||
               mime == "application/rtf" || mime == "application/epub+zip" || mime == "application/json" ||
               mime == "application/xml" || mime == "text/markdown";
    return true;
}

std::vector<Recent> parse_recent(std::string_view xbel) {
    std::vector<Recent> out;
    size_t at = 0;
    while ((at = xbel.find("<bookmark ", at)) != std::string_view::npos) {
        const size_t end = xbel.find('>', at);
        if (end == std::string_view::npos)
            break;
        const std::string_view tag = xbel.substr(at, end - at);
        at = end;
        const std::string href = unescape(attribute(tag, "href"));
        if (!href.starts_with("file://"))
            continue;
        Recent r{percent_decode(std::string_view(href).substr(7)), 0};
        for (const char* stamp : {"visited", "modified", "added"})
            r.when = std::max(r.when, parse_time(attribute(tag, stamp)));
        out.push_back(std::move(r));
    }
    std::ranges::stable_sort(out, [](const Recent& a, const Recent& b) { return a.when > b.when; });
    // One row per file.
    std::vector<Recent> unique;
    for (Recent& r : out)
        if (std::ranges::none_of(unique, [&](const Recent& u) { return u.path == r.path; }))
            unique.push_back(std::move(r));
    return unique;
}

namespace {

// The program an exec line runs, without quotes or a path.
std::string exec_binary(std::string_view exec) {
    size_t a = exec.find_first_not_of(" '\"");
    if (a == std::string_view::npos)
        return {};
    size_t b = exec.find_first_of(" '\"", a);
    std::string_view word = exec.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a);
    if (const size_t slash = word.rfind('/'); slash != std::string_view::npos)
        word = word.substr(slash + 1);
    return std::string(word);
}

std::vector<std::string> lowered(const std::vector<std::string>& names) {
    std::vector<std::string> out;
    for (const std::string& n : names)
        if (!n.empty())
            out.push_back(lower(n));
    return out;
}

bool is_app(std::string_view atag, const std::vector<std::string>& wanted, std::string_view binary) {
    const std::string name = lower(unescape(attribute(atag, "name")));
    return (!name.empty() && std::ranges::find(wanted, name) != wanted.end()) ||
           (!binary.empty() && exec_binary(unescape(attribute(atag, "exec"))) == binary);
}

} // namespace

std::vector<Recent> recent_for_app(std::string_view xbel, const std::vector<std::string>& names,
                                   std::string_view binary) {
    const std::vector<std::string> wanted = lowered(names);
    std::vector<Recent> out;
    size_t at = 0;
    while ((at = xbel.find("<bookmark ", at)) != std::string_view::npos) {
        const size_t end = xbel.find('>', at);
        if (end == std::string_view::npos)
            break;
        const std::string_view tag = xbel.substr(at, end - at);
        if (tag.ends_with('/')) {  // no applications inside
            at = end;
            continue;
        }
        size_t close = xbel.find("</bookmark>", end);
        if (close == std::string_view::npos)
            close = xbel.size();
        const std::string_view body = xbel.substr(end, close - end);
        at = close;
        const std::string href = unescape(attribute(tag, "href"));
        if (!href.starts_with("file://"))
            continue;
        long long when = -1;
        size_t app = 0;
        while ((app = body.find("<bookmark:application ", app)) != std::string_view::npos) {
            const size_t app_end = body.find('>', app);
            if (app_end == std::string_view::npos)
                break;
            const std::string_view atag = body.substr(app, app_end - app);
            app = app_end;
            if (is_app(atag, wanted, binary))
                when = std::max(when, parse_time(attribute(atag, "modified")));
        }
        if (when >= 0)
            out.push_back({percent_decode(std::string_view(href).substr(7)), when});
    }
    std::ranges::stable_sort(out, [](const Recent& a, const Recent& b) { return a.when > b.when; });
    std::vector<Recent> unique;
    for (Recent& r : out)
        if (std::ranges::none_of(unique, [&](const Recent& u) { return u.path == r.path; }))
            unique.push_back(std::move(r));
    return unique;
}

std::string forget_app(std::string_view xbel, const std::vector<std::string>& names, std::string_view binary) {
    const std::vector<std::string> wanted = lowered(names);
    std::string out;
    size_t copied = 0, at = 0;
    while ((at = xbel.find("<bookmark ", at)) != std::string_view::npos) {
        const size_t end = xbel.find('>', at);
        if (end == std::string_view::npos)
            break;
        if (xbel.substr(at, end - at).ends_with('/')) {
            at = end;
            continue;
        }
        size_t close = xbel.find("</bookmark>", end);
        if (close == std::string_view::npos)
            break;
        close += 11;
        // This bookmark, without the app's lines; how many apps are left.
        std::string kept;
        int others = 0, mine = 0;
        size_t from = at, app = end;
        const std::string_view block = xbel.substr(0, close);
        while ((app = block.find("<bookmark:application ", app)) != std::string_view::npos) {
            const size_t app_end = block.find('>', app);
            if (app_end == std::string_view::npos)
                break;
            if (is_app(block.substr(app, app_end - app), wanted, binary)) {
                ++mine;
                // The whole line, indent and newline too.
                size_t line = block.rfind('\n', app);
                line = line == std::string_view::npos || line < from ? app : line;
                kept.append(block.substr(from, line - from));
                from = app_end + 1;
            } else {
                ++others;
            }
            app = app_end;
        }
        kept.append(block.substr(from, close - from));
        if (mine) {
            out.append(xbel.substr(copied, at - copied));
            if (others) {
                out.append(kept);
            } else if (out.ends_with("  ")) {
                while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
                    out.pop_back();
                if (close < xbel.size() && xbel[close] == '\n')
                    ++close;
            }
            copied = close;
        }
        at = close;
    }
    out.append(xbel.substr(copied));
    return out;
}

} // namespace atrium::file_search
