#include "doctor_core.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>

namespace atrium::doctor {

namespace {

template <typename T>
bool read(std::string_view d, size_t at, T& out) {
    if (at > d.size() || d.size() - at < sizeof(T))
        return false;
    std::memcpy(&out, d.data() + at, sizeof(T));
    return true;
}

struct Section {
    std::string_view name, data;
};

// A 64-bit little-endian ELF file's section `name`.
Section find_section(std::string_view elf, std::string_view name) {
    if (elf.size() < 64 || elf.substr(0, 4) != "\x7f" "ELF" || elf[4] != 2 || elf[5] != 1)
        return {};
    uint64_t shoff = 0;
    uint16_t shentsize = 0, shnum = 0, shstrndx = 0;
    if (!read(elf, 0x28, shoff) || !read(elf, 0x3a, shentsize) || !read(elf, 0x3c, shnum) ||
        !read(elf, 0x3e, shstrndx) || shentsize < 64 || shstrndx >= shnum)
        return {};
    auto header = [&](unsigned i, uint32_t& name_at, uint64_t& offset, uint64_t& size) {
        const size_t at = shoff + size_t(i) * shentsize;
        return read(elf, at, name_at) && read(elf, at + 24, offset) && read(elf, at + 32, size) &&
               offset <= elf.size() && size <= elf.size() - offset;
    };
    uint32_t name_at;
    uint64_t str_off, str_size;
    if (!header(shstrndx, name_at, str_off, str_size))
        return {};
    const std::string_view names = elf.substr(str_off, str_size);
    for (unsigned i = 0; i < shnum; ++i) {
        uint64_t off, size;
        if (!header(i, name_at, off, size) || name_at >= names.size())
            continue;
        const std::string_view n = names.substr(name_at);
        if (n.substr(0, n.find('\0')) == name)
            return {name, elf.substr(off, size)};
    }
    return {};
}

bool is_platform_dir(std::string_view dir) {
    static constexpr std::array<std::string_view, 5> kDirs = {
        "platforms", "platformthemes", "platforminputcontexts", "xcbglintegrations", "egldeviceintegrations"};
    return std::ranges::find(kDirs, dir) != kDirs.end();
}

std::string version(QtVersion v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor);
}

} // namespace

QtVersion plugin_qt(std::string_view elf) {
    const Section note = find_section(elf, ".note.qt.metadata");
    // namesz, descsz, type, then the name padded to 4, then the header.
    uint32_t namesz = 0, descsz = 0;
    if (!read(note.data, 0, namesz) || !read(note.data, 4, descsz) || descsz < 4)
        return {};
    const size_t name_end = 12 + ((namesz + 3) & ~3u);
    if (note.data.substr(12, namesz).substr(0, 11) != "qt-project!" || note.data.size() < name_end + 4)
        return {};
    return {static_cast<unsigned char>(note.data[name_end + 1]), static_cast<unsigned char>(note.data[name_end + 2])};
}

bool uses_private_qt(std::string_view elf) {
    return find_section(elf, ".dynstr").data.find("Qt_6_PRIVATE_API") != std::string_view::npos;
}

QtVersion qt_from_soname(std::string_view name) {
    const size_t so = name.find(".so.");
    if (so == std::string_view::npos)
        return {};
    std::string_view rest = name.substr(so + 4);
    QtVersion v;
    auto [p, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), v.major);
    if (ec != std::errc{} || p == rest.data() + rest.size() || *p != '.')
        return {};
    ++p;
    auto [q, ec2] = std::from_chars(p, rest.data() + rest.size(), v.minor);
    if (ec2 != std::errc{} || q == p)
        return {};
    return v;
}

std::string plugin_problem(QtVersion built, QtVersion installed, std::string_view dir, bool private_api,
                           Source source) {
    if (!built.known() || !installed.known() || built.major != installed.major || built == installed)
        return {};
    if (built.minor > installed.minor)
        return "built for Qt " + version(built) + ", newer than the installed " + version(installed) +
               ": Qt won't load it";
    if (is_platform_dir(dir))
        return "built for Qt " + version(built) + ": Qt " + version(installed) +
               " only loads platform plugins built for it";
    if (private_api && source != Source::Repo)
        return "built for Qt " + version(built) + " on Qt's internals, which change in " + version(installed);
    return {};
}

std::vector<std::pair<std::string, std::string>> parse_owners(std::string_view output) {
    std::vector<std::pair<std::string, std::string>> out;
    constexpr std::string_view kOwned = " is owned by ";
    while (!output.empty()) {
        const size_t nl = output.find('\n');
        const std::string_view line = output.substr(0, nl);
        output = nl == std::string_view::npos ? std::string_view{} : output.substr(nl + 1);
        const size_t at = line.rfind(kOwned);
        if (at == std::string_view::npos)
            continue;
        std::string_view rest = line.substr(at + kOwned.size());
        out.emplace_back(std::string(line.substr(0, at)), std::string(rest.substr(0, rest.find(' '))));
    }
    return out;
}

std::vector<std::string> ldd_problems(std::string_view output) {
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        if (std::ranges::find(out, s) == out.end())
            out.push_back(std::move(s));
    };
    while (!output.empty()) {
        const size_t nl = output.find('\n');
        std::string_view line = output.substr(0, nl);
        output = nl == std::string_view::npos ? std::string_view{} : output.substr(nl + 1);
        if (line.find("not found") == std::string_view::npos)
            continue;
        // "\tlibfoo.so.3 => not found"
        if (const size_t arrow = line.find(" => not found"); arrow != std::string_view::npos) {
            std::string_view lib = line.substr(0, arrow);
            lib.remove_prefix(std::min(lib.find_first_not_of(" \t"), lib.size()));
            add(std::string(lib) + " is missing");
            continue;
        }
        // "/usr/bin/x: /usr/lib/libQt6Core.so.6: version `Qt_6.12' not found (required by /usr/bin/x)"
        const size_t ver = line.find(": version `");
        if (ver == std::string_view::npos)
            continue;
        std::string_view lib = line.substr(0, ver);
        lib = lib.substr(lib.rfind('/') == std::string_view::npos ? 0 : lib.rfind('/') + 1);
        std::string_view tag = line.substr(ver + 11);
        tag = tag.substr(0, tag.find('\''));
        add("needs " + std::string(tag) + " of " + std::string(lib));
    }
    return out;
}

std::vector<std::string> packages(const std::vector<Finding>& findings) {
    std::vector<std::string> out;
    for (const Finding& f : findings) {
        const std::string name = f.package.empty() ? f.file : f.package;
        if (std::ranges::find(out, name) == out.end())
            out.push_back(name);
    }
    return out;
}

std::string brief(const std::vector<Finding>& findings) {
    const std::vector<std::string> names = packages(findings);
    if (names.empty())
        return {};
    std::string list;
    for (size_t i = 0; i < names.size(); ++i)
        list += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    return list + (names.size() == 1 ? " doesn't" : " don't") +
           " work with what's installed now: run atrium-doctor to see why, atrium-doctor --fix to fix it";
}

std::string remedy(Source source, bool newer_release) {
    switch (source) {
    case Source::Atrium:
        return newer_release ? "install the latest release"
                             : "build the latest atrium from source and install it (no newer release yet)";
    case Source::Aur: return "rebuild it from the AUR";
    case Source::Repo:
        return "update the system (sudo pacman -Syu); if nothing's newer, its repository hasn't rebuilt it for "
               "this Qt yet";
    }
    return {};
}

} // namespace atrium::doctor
