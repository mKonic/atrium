#include "updates_core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace atrium::updates {

PackageId parse_package_id(std::string_view id) {
    std::array<std::string_view, 4> parts;
    for (size_t i = 0; i < parts.size(); ++i) {
        const size_t semi = id.find(';');
        if (semi == std::string_view::npos && i < parts.size() - 1)
            return {};
        parts[i] = id.substr(0, semi);
        id.remove_prefix(semi == std::string_view::npos ? id.size() : semi + 1);
    }
    if (parts[0].empty())
        return {};
    return {std::string(parts[0]), std::string(parts[1]), std::string(parts[2]), std::string(parts[3])};
}

std::string doing(unsigned info) {
    switch (info) {
    case pk::kInfoDownloading: return "Downloading";
    case pk::kInfoPreparing: return "Preparing";
    case pk::kInfoDecompressing: return "Unpacking";
    case pk::kInfoUpdating:
    case pk::kInfoInstalling: return "Installing";
    case pk::kInfoRemoving: return "Removing";
    case pk::kInfoCleanup: return "Cleaning up";
    default: return "";
    }
}

bool needs_restart(std::string_view name) {
    static constexpr std::string_view whole[] = {"systemd", "glibc", "linux-firmware", "mesa", "dbus",
                                                 "dbus-broker", "amd-ucode", "intel-ucode"};
    for (std::string_view w : whole)
        if (name == w)
            return true;
    // linux, linux-lts, linux-cachyos, linux-zen …; nvidia, nvidia-open, nvidia-utils …
    const bool kernel = (name == "linux" || name.starts_with("linux-")) && !name.starts_with("linux-api") &&
                        !name.starts_with("linux-tools") && !name.starts_with("linux-firmware-");
    return kernel || name.starts_with("nvidia");
}

std::string summary(int count, int security) {
    if (count <= 0)
        return "No updates";
    std::string s = std::to_string(count) + (count == 1 ? " update" : " updates");
    if (security > 0)
        s += ", " + std::to_string(security) + " for security";
    return s;
}

namespace {

std::string strip_ansi(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i])))
                ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::vector<std::string> fields(std::string_view line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty())
                out.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

struct Semver {
    std::vector<long> core;
    std::vector<std::string> pre;  // empty: a release
    bool ok = false;
};

Semver parse_semver(std::string_view t) {
    Semver v;
    if (t.starts_with('v'))
        t.remove_prefix(1);
    if (const size_t plus = t.find('+'); plus != std::string_view::npos)
        t = t.substr(0, plus);
    std::string_view core = t, pre;
    if (const size_t dash = t.find('-'); dash != std::string_view::npos)
        core = t.substr(0, dash), pre = t.substr(dash + 1);
    size_t start = 0;
    while (start <= core.size()) {
        const size_t dot = core.find('.', start);
        const std::string part(core.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (part.empty() || !std::ranges::all_of(part, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return v;
        v.core.push_back(std::stol(part));
        if (dot == std::string_view::npos)
            break;
        start = dot + 1;
    }
    if (v.core.size() != 3)
        return v;
    start = 0;
    while (!pre.empty() && start <= pre.size()) {
        const size_t dot = pre.find('.', start);
        v.pre.emplace_back(pre.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos)
            break;
        start = dot + 1;
    }
    v.ok = true;
    return v;
}

bool numeric(const std::string& s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

} // namespace

std::vector<Upgrade> parse_upgrades(std::string_view text) {
    std::vector<Upgrade> out;
    size_t start = 0;
    while (start < text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string line = strip_ansi(text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
        start = nl == std::string_view::npos ? text.size() : nl + 1;
        const auto f = fields(line);
        // "name old -> new"; one pacman is told to ignore ("[ignored]") isn't one.
        if (f.size() >= 4 && f[2] == "->" && !(f.size() > 4 && f[4] == "[ignored]"))
            out.push_back({f[0], f[1], f[3]});
    }
    return out;
}

bool parse_pacman_step(std::string_view line, Step& step) {
    const std::string l = strip_ansi(line);
    // "(3/10) upgrading firefox  [####]  100%"
    int done = 0, total = 0;
    char verb[32] = {}, name[256] = {};
    if (std::sscanf(l.c_str(), " (%d/%d) %31s %255s", &done, &total, verb, name) != 4 || total <= 0)
        return false;
    std::string v = verb;
    if (v != "upgrading" && v != "installing" && v != "removing" && v != "reinstalling" && v != "downgrading")
        return false;
    v[0] = char(std::toupper(static_cast<unsigned char>(v[0])));
    step = {done, total, v + " " + name};
    return true;
}

int compare_tags(std::string_view a, std::string_view b) {
    const Semver x = parse_semver(a), y = parse_semver(b);
    if (!x.ok || !y.ok)
        return x.ok - y.ok;
    for (size_t i = 0; i < 3; ++i)
        if (x.core[i] != y.core[i])
            return x.core[i] < y.core[i] ? -1 : 1;
    // A pre-release comes before its release.
    if (x.pre.empty() != y.pre.empty())
        return x.pre.empty() ? 1 : -1;
    for (size_t i = 0; i < std::min(x.pre.size(), y.pre.size()); ++i) {
        const std::string &p = x.pre[i], &q = y.pre[i];
        if (p == q)
            continue;
        if (numeric(p) && numeric(q))
            return std::stol(p) < std::stol(q) ? -1 : 1;
        if (numeric(p) != numeric(q))
            return numeric(p) ? -1 : 1;
        return p < q ? -1 : 1;
    }
    return x.pre.size() == y.pre.size() ? 0 : x.pre.size() < y.pre.size() ? -1 : 1;
}

bool release_newer(std::string_view tag, std::string_view build) {
    if (!parse_semver(tag).ok)
        return false;
    // "v0.1.0-5-gabc1234" (git describe past a tag) is that tag plus work:
    // the tag itself is what it's compared as, and equal is not newer.
    std::string b(build);
    if (b.ends_with("-dirty"))
        b.resize(b.size() - 6);
    const size_t g = b.rfind("-g");
    if (g != std::string::npos) {
        const size_t n = b.rfind('-', g - 1);
        if (n != std::string::npos && numeric(b.substr(n + 1, g - n - 1)))
            b.resize(n);
    }
    if (!parse_semver(b).ok)
        return true;  // before any tag: every release is newer
    return compare_tags(tag, b) > 0;
}

std::string pkgver(std::string_view name, long build) {
    std::string n(name);
    if (n.ends_with("-dirty"))
        n.resize(n.size() - 6);
    if (!n.starts_with('v') || n.size() < 2 || !std::isdigit(static_cast<unsigned char>(n[1])))
        return "0.r" + std::to_string(build - 10000) + "." + n;
    n.erase(0, 1);
    // -N-gHASH at the end → .rN.gHASH
    const size_t g = n.rfind("-g");
    if (g != std::string::npos) {
        const size_t d = n.rfind('-', g - 1);
        if (d != std::string::npos && numeric(n.substr(d + 1, g - d - 1)))
            n = n.substr(0, d) + ".r" + n.substr(d + 1, g - d - 1) + ".g" + n.substr(g + 2);
    }
    // The first remaining hyphen goes (1.4.0rc.1), any other becomes a dot.
    if (const size_t h = n.find('-'); h != std::string::npos)
        n.erase(h, 1);
    std::ranges::replace(n, '-', '.');
    return n;
}

} // namespace atrium::updates
