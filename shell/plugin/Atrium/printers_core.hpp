#pragma once
// CUPS as its command-line tools report it (lpstat, run with LC_ALL=C),
// parsed without Qt.

#include <string>
#include <string_view>
#include <vector>

namespace atrium::printers {

struct Printer {
    std::string name;
    std::string description;  // "Description:" from lpstat -l, else ""
    std::string state;        // "idle", "printing", "stopped"
    bool enabled = true;
};

struct Job {
    std::string id;       // "Office-12"
    std::string printer;  // "Office"
    std::string user;
    long long size = 0;   // bytes
};

// `lpstat -l -p`.
std::vector<Printer> parse_printers(std::string_view text);
// `lpstat -d`: the default's name, "" for none.
std::string parse_default(std::string_view text);
// `lpstat -o`, given the printers' names (job ids are NAME-NUMBER, and
// names may hold dashes).
std::vector<Job> parse_jobs(std::string_view text, const std::vector<Printer>& printers);

} // namespace atrium::printers
