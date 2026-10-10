#include "doctor_core.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using namespace atrium::doctor;

namespace {

// A 64-bit ELF file with the given sections (name, contents), as a plugin's
// headers lay them out: the section name table last.
std::string elf(std::vector<std::pair<std::string, std::string>> sections) {
    std::string names(1, '\0');
    std::vector<uint32_t> name_at;
    sections.emplace_back(".shstrtab", "");
    for (auto& [name, _] : sections) {
        name_at.push_back(uint32_t(names.size()));
        names += name + '\0';
    }
    sections.back().second = names;

    std::string out(64, '\0');
    std::memcpy(out.data(), "\x7f" "ELF\x02\x01\x01", 7);
    std::vector<std::pair<uint64_t, uint64_t>> placed;
    for (auto& [_, data] : sections) {
        placed.emplace_back(out.size(), data.size());
        out += data;
    }
    const uint64_t shoff = out.size();
    const uint16_t shentsize = 64, shnum = uint16_t(sections.size() + 1), shstrndx = uint16_t(sections.size());
    std::memcpy(out.data() + 0x28, &shoff, 8);
    std::memcpy(out.data() + 0x3a, &shentsize, 2);
    std::memcpy(out.data() + 0x3c, &shnum, 2);
    std::memcpy(out.data() + 0x3e, &shstrndx, 2);
    out += std::string(64, '\0');  // section 0 is null
    for (size_t i = 0; i < sections.size(); ++i) {
        std::string sh(64, '\0');
        std::memcpy(sh.data(), &name_at[i], 4);
        std::memcpy(sh.data() + 24, &placed[i].first, 8);
        std::memcpy(sh.data() + 32, &placed[i].second, 8);
        out += sh;
    }
    return out;
}

// qplugin.h's note: name "qt-project!", then version, Qt major, minor, arch.
std::string qt_note(int major, int minor) {
    std::string n(12, '\0');
    const uint32_t namesz = 12, descsz = 4, type = 0x74510001;
    std::memcpy(n.data(), &namesz, 4);
    std::memcpy(n.data() + 4, &descsz, 4);
    std::memcpy(n.data() + 8, &type, 4);
    n += std::string("qt-project!\0", 12);
    n += std::string{char(1), char(major), char(minor), char(4)};
    return n;
}

} // namespace

TEST(Doctor, ReadsTheQtAPluginWasBuiltFor) {
    EXPECT_EQ(plugin_qt(elf({{".note.qt.metadata", qt_note(6, 11)}})), (QtVersion{6, 11}));
    EXPECT_EQ(plugin_qt(elf({{".text", "code"}, {".note.qt.metadata", qt_note(6, 12)}})), (QtVersion{6, 12}));
    // Not a Qt plugin, not ELF, cut short: unknown, never a crash.
    EXPECT_FALSE(plugin_qt(elf({{".text", "code"}})).known());
    EXPECT_FALSE(plugin_qt("#!/bin/sh\n").known());
    const std::string whole = elf({{".note.qt.metadata", qt_note(6, 12)}});
    for (size_t cut : {size_t(10), size_t(70), whole.size() - 40})
        EXPECT_FALSE(plugin_qt(std::string_view(whole).substr(0, cut)).known()) << cut;
}

TEST(Doctor, FindsQtPrivateApiInTheDynamicStrings) {
    EXPECT_TRUE(uses_private_qt(elf({{".dynstr", std::string("\0Qt_6\0Qt_6_PRIVATE_API\0", 23)}})));
    EXPECT_FALSE(uses_private_qt(elf({{".dynstr", std::string("\0Qt_6\0", 6)}})));
    // Only the dynamic strings count, not text that happens to say it.
    EXPECT_FALSE(uses_private_qt(elf({{".rodata", "Qt_6_PRIVATE_API"}, {".dynstr", std::string("\0Qt_6\0", 6)}})));
}

TEST(Doctor, InstalledQtFromItsLibrary) {
    EXPECT_EQ(qt_from_soname("libQt6Core.so.6.12.0"), (QtVersion{6, 12}));
    EXPECT_EQ(qt_from_soname("libQt6Core.so.6.9.3"), (QtVersion{6, 9}));
    EXPECT_FALSE(qt_from_soname("libQt6Core.so.6").known());
    EXPECT_FALSE(qt_from_soname("libQt6Core.so").known());
}

TEST(Doctor, WhichPluginsTheInstalledQtWontRun) {
    const QtVersion qt{6, 12};
    // Built for it: fine wherever it comes from.
    EXPECT_EQ(plugin_problem({6, 12}, qt, "platformthemes", true, Source::Aur), "");
    // Built for a newer Qt: refused, whoever made it.
    EXPECT_NE(plugin_problem({6, 13}, qt, "imageformats", false, Source::Repo), "");
    // A platform plugin from another minor: refused (qtengine after an upgrade).
    EXPECT_NE(plugin_problem({6, 11}, qt, "platformthemes", false, Source::Aur), "");
    EXPECT_NE(plugin_problem({6, 11}, qt, "platformthemes", false, Source::Repo), "");
    // On Qt's internals from another minor: atrium's lock screen, an AUR
    // package; a repository's own are its to rebuild (Arch keeps working ones).
    EXPECT_NE(plugin_problem({6, 11}, qt, "wayland-shell-integration", true, Source::Atrium), "");
    EXPECT_NE(plugin_problem({6, 11}, qt, "kf6", true, Source::Aur), "");
    EXPECT_EQ(plugin_problem({6, 11}, qt, "kf6", true, Source::Repo), "");
    // An older ordinary plugin on public API: Qt loads it.
    EXPECT_EQ(plugin_problem({6, 11}, qt, "imageformats", false, Source::Aur), "");
    // Another major (a Qt 5 plugin), or nothing known: not this check's.
    EXPECT_EQ(plugin_problem({5, 15}, qt, "platformthemes", false, Source::Aur), "");
    EXPECT_EQ(plugin_problem({}, qt, "platformthemes", false, Source::Aur), "");
}

TEST(Doctor, WhatLddSaysIsWrong) {
    const char* out = "\tlinux-vdso.so.1 (0x00007ffd)\n"
                      "\tlibdisplay-info.so.2 => not found\n"
                      "\tlibQt6Core.so.6 => /usr/lib/libQt6Core.so.6 (0x7f)\n"
                      "/usr/bin/atrium-shell: /usr/lib/libQt6Core.so.6: version `Qt_6.12' not found (required by /usr/bin/atrium-shell)\n"
                      "/usr/bin/atrium-shell: /usr/lib/libQt6Core.so.6: version `Qt_6.12' not found (required by /usr/lib/atrium/x.so)\n";
    const auto problems = ldd_problems(out);
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_EQ(problems[0], "libdisplay-info.so.2 is missing");
    EXPECT_EQ(problems[1], "needs Qt_6.12 of libQt6Core.so.6");
    EXPECT_TRUE(ldd_problems("\tlibc.so.6 => /usr/lib/libc.so.6 (0x7f)\n").empty());
}

TEST(Doctor, OwnersFromPacman) {
    const auto owners = parse_owners("/usr/lib/a.so is owned by qtengine 0.2.2-1\n"
                                     "/usr/lib/b c.so is owned by atrium-git 0.2.0.r24.g609dbba-1\n");
    ASSERT_EQ(owners.size(), 2u);
    EXPECT_EQ(owners[0], (std::pair<std::string, std::string>{"/usr/lib/a.so", "qtengine"}));
    EXPECT_EQ(owners[1], (std::pair<std::string, std::string>{"/usr/lib/b c.so", "atrium-git"}));
}

TEST(Doctor, OneLineNamesEachPackageOnce) {
    EXPECT_EQ(brief({}), "");
    const std::vector<Finding> one = {{"qtengine", Source::Aur, "/a.so", "x"}, {"qtengine", Source::Aur, "/b.so", "y"}};
    EXPECT_EQ(brief(one).rfind("qtengine doesn't work", 0), 0u);
    std::vector<Finding> three = one;
    three.push_back({"atrium-git", Source::Atrium, "/c.so", "z"});
    three.push_back({"kwindowsystem", Source::Repo, "/d.so", "z"});
    EXPECT_EQ(brief(three).rfind("qtengine, atrium-git and kwindowsystem don't work", 0), 0u);
}

TEST(Doctor, FixDependsOnWhereItCameFrom) {
    EXPECT_NE(remedy(Source::Atrium, true).find("release"), std::string::npos);
    EXPECT_NE(remedy(Source::Atrium, false).find("source"), std::string::npos);
    EXPECT_NE(remedy(Source::Aur, false).find("AUR"), std::string::npos);
    EXPECT_NE(remedy(Source::Repo, false).find("pacman -Syu"), std::string::npos);
}
