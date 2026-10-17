#pragma once
// What atrium-doctor asks the system: other programs' output, processes in
// /proc, D-Bus names' owners. Nothing linked but the C++ runtime.

#include <string>
#include <vector>

namespace atrium::doctor {

// Runs `argv`, its output captured (stderr too when `errors`); "" when it
// can't start. `status` gets its exit status.
std::string capture(const std::vector<std::string>& argv, int* status = nullptr, bool errors = false);

// Runs `argv` on this terminal, in `dir`; true when it succeeds.
bool run(const std::vector<std::string>& argv, const std::string& dir = {}, const std::vector<std::string>& env = {});

std::vector<std::string> lines(const std::string& text);
std::string trim(std::string s);

// The package that owns `file` ("" for none).
std::string owner(const std::string& file);

// A process of this user's.
struct Process {
    int pid = 0;
    std::string exe;     // what it runs, as /proc/PID/exe says
    bool stale = false;  // that file was replaced since it started (an update)
};

// This user's processes running `/usr/bin/NAME` (or /usr/lib/NAME): never
// a build tree's, as a test box runs.
std::vector<Process> processes(const std::string& name);

// What `pid` runs, as a Process; pid 0 when it's gone.
Process process(int pid);

// Seconds since `pid` started; -1 when it's gone.
long long age(int pid);

// `key` in `pid`'s environment; "" when unset.
std::string environment(int pid, const std::string& key);

// The systemd unit `pid` runs in ("session-5.scope", "pipewire.service").
std::string unit(int pid);

// The pid owning a session bus name; 0 when nobody owns it.
int bus_owner(const std::string& name);

// The session's journal for one program (SYSLOG_IDENTIFIER) since
// `since` (seconds since the epoch), each line's message alone.
std::vector<std::string> journal(const std::string& identifier, long long since, const std::string& priority = {});

} // namespace atrium::doctor
