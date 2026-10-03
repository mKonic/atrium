#pragma once
// CUPS as its command-line tools report it (lpstat, run with LC_ALL=C),
// parsed without Qt.

#include <optional>
#include <span>
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

// A printer's option as `lpoptions -p NAME -l` lists it:
// "PageSize/Media Size: Letter *A4 A5" (the starred one is set).
struct Option {
    std::string key;    // "PageSize"
    std::string label;  // "Media Size"
    std::vector<std::string> choices;
    std::string current;
};
std::vector<Option> parse_options(std::string_view text);

// A paper size by its PPD name ("A4"): its PWG name ("iso_a4", what GTK
// calls it), the name to show, and its size in millimetres. Nothing for
// one not known here.
struct Paper {
    std::string_view ppd, pwg, display;
    double width = 0, height = 0;
};
const Paper* paper_by_ppd(std::string_view ppd);
// All of them, for a PDF (which takes any).
std::span<const Paper> papers();
const Paper* paper_by_pwg(std::string_view pwg);

// Pages as typed ("1-3, 5", from 1) as GTK's print settings keep them
// ("0-2,4", from 0); nothing when they don't read as pages.
std::optional<std::string> zero_based_ranges(std::string_view typed);

// What to print, as the print dialog leaves it (GTK's print settings).
struct PrintJob {
    std::string printer;
    std::string title;
    int copies = 1;
    std::string ranges;       // "1-3,5"; empty: all
    std::string page_set;     // "odd", "even"; empty: all
    std::string paper;        // PPD name; empty: the printer's own
    bool landscape = false;
    std::string duplex;       // PPD Duplex: "None", "DuplexNoTumble", "DuplexTumble"; empty: the printer's
    std::string color_model;  // PPD ColorModel ("Gray", "RGB"); empty: the printer's
    bool collate = true;
    bool reverse = false;
    int number_up = 1;
};
// `lp`'s arguments for it (no file: the document comes on stdin).
std::vector<std::string> lp_args(const PrintJob& job);

} // namespace atrium::printers
