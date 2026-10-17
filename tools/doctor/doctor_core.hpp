#pragma once
// atrium-doctor's reasoning, without the system: what a Qt plugin was built
// for, whether the installed Qt will run it, what ldd complains about, how a
// finding is told, and the health report's sections (as Neovim's
// :checkhealth and flutter doctor tell theirs).

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium::doctor {

struct QtVersion {
    int major = 0, minor = 0;
    bool known() const { return major > 0; }
    bool operator==(const QtVersion&) const = default;
};

// The Qt a plugin was built for, from its .note.qt.metadata (qplugin.h's
// header after the "qt-project!" name: format version, Qt major, Qt minor,
// architecture requirements). Unknown when `elf` isn't a 64-bit ELF file or
// has no such note (not a Qt plugin).
QtVersion plugin_qt(std::string_view elf);

// Whether `elf` uses Qt's private API (its symbols are versioned
// Qt_6_PRIVATE_API), which changes between Qt's minor versions.
bool uses_private_qt(std::string_view elf);

// "libQt6Core.so.6.12.0" (where libQt6Core.so.6 points) → 6.12.
QtVersion qt_from_soname(std::string_view name);

// `pacman -Qo FILE...`'s answer: each file's package, by its path ("/usr/lib/x.so
// is owned by foo 1.0-1"); files no package owns are left out.
std::vector<std::pair<std::string, std::string>> parse_owners(std::string_view output);

// Where a package comes from, which decides how it's fixed.
enum class Source { Atrium, Aur, Repo };

// Why a plugin built for `built` won't work under the `installed` Qt, or ""
// when it will. `dir` is its folder under plugins/ ("platformthemes"): Qt
// loads platform plugins only when built for its own minor version, any
// plugin only when built for it or an older one. A plugin on Qt's private
// API (`private_api`) loads but may run on internals that changed: told for
// atrium and the AUR only, as a repository rebuilds its own packages when
// their use of those internals breaks (and Arch leaves the rest as they are).
std::string plugin_problem(QtVersion built, QtVersion installed, std::string_view dir, bool private_api,
                           Source source);

// What `ldd FILE` says is wrong: libraries not found and symbol versions
// missing ("needs Qt_6.12 of libQt6Core.so.6"), each once.
std::vector<std::string> ldd_problems(std::string_view output);

struct Finding {
    std::string package;  // its owner; "" when no package owns the file
    Source source = Source::Repo;
    std::string file;
    std::string problem;
};

// The packages the findings name, each once, in order.
std::vector<std::string> packages(const std::vector<Finding>& findings);

// One line for pacman's output and the login notification:
// "qtengine and atrium-git were built for another Qt: run atrium-doctor --fix".
std::string brief(const std::vector<Finding>& findings);

// How a package gets fixed, as atrium-doctor --fix will do it.
std::string remedy(Source source, bool newer_release);

// --- the health report ---------------------------------------------------------

// Worst last: a section is as bad as its worst line. Skip: not looked at
// (outside an atrium session, say).
enum class Level { Ok, Info, Skip, Warn, Error };

struct Item {
    Level level = Level::Info;
    std::string text;
    std::string advice;  // what to do about it ("" for none)
};

struct Report {
    std::string name;     // what picks it on the command line: "portals"
    std::string title;    // "Portals"
    std::string summary;  // beside the title: "xdg-desktop-portal answers"
    std::vector<Item> items;
    bool skipped = false;

    void ok(std::string text) { items.push_back({Level::Ok, std::move(text), {}}); }
    void info(std::string text) { items.push_back({Level::Info, std::move(text), {}}); }
    void warn(std::string text, std::string advice = {}) {
        items.push_back({Level::Warn, std::move(text), std::move(advice)});
    }
    void error(std::string text, std::string advice = {}) {
        items.push_back({Level::Error, std::move(text), std::move(advice)});
    }
    void skip(std::string why) {
        skipped = true;
        summary = std::move(why);
    }
    Level status() const;
};

// The report for a terminal: a line per section ([✓] [!] [✗] [-]), under
// it what's wrong and what to do (and, `verbose`, everything looked at),
// then one line on the whole. `color`: ANSI colors.
std::string render(const std::vector<Report>& reports, bool verbose, bool color);

// "v0.3.0-37-g90a9a21" or "0.3.0.r37.g90a9a21-1" → "90a9a21": the commit a
// build was made from, "" when it doesn't say.
std::string build_commit(std::string_view version);

// 8000 → "2h 13m", 90 → "1m", 30 → "30s".
std::string duration(long long seconds);

// Log lines alike but for their numbers, counted: the most often first,
// each told by its first line.
struct Tally {
    std::string example;
    int count = 0;
};
std::vector<Tally> tally(const std::vector<std::string>& lines);

// From `coredumpctl info`: the crashed thread's innermost frame, as
// "pw_thread_loop_stop (libpipewire-0.3.so.0)"; "" when it has none.
std::string top_frame(std::string_view info);

// A QML error worth a bug report (a TypeError, a binding to an undefined
// property...), not Qt's chatter.
bool qml_error(std::string_view line);

} // namespace atrium::doctor
