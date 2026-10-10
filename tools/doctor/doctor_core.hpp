#pragma once
// atrium-doctor's reasoning, without the system: what a Qt plugin was built
// for, whether the installed Qt will run it, what ldd complains about, and
// how a finding is told.

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

} // namespace atrium::doctor
