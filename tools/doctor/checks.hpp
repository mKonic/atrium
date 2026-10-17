#pragma once
// atrium-doctor's checks: one report each, on the installed packages and the
// running session.

#include "doctor_core.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace atrium::doctor {

struct Context {
    std::string atrium;  // the package /usr/bin/atrium belongs to
    std::set<std::string> foreign;  // packages from no repository (pacman -Qm)
};
Context context();

// What doesn't work with what's installed: atrium's binaries' libraries,
// every Qt plugin's Qt (what --fix fixes).
std::vector<Finding> examine(const Context& c);

// The atrium session running, when there is one.
struct Session {
    int pid = 0;            // the compositor; 0: none
    long long started = 0;  // seconds since the epoch
    std::string version;    // "v0.3.0-37-g90a9a21"
    std::string why;        // why there's none
};
Session session();

struct Check {
    const char* name;
    const char* about;
    std::function<Report(const Context&, const Session&)> run;
};
const std::vector<Check>& checks();

} // namespace atrium::doctor
