#include "printers_core.hpp"

#include <sstream>

namespace atrium::printers {

namespace {

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        out.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos)
            break;
        text.remove_prefix(nl + 1);
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

} // namespace

std::vector<Printer> parse_printers(std::string_view text) {
    std::vector<Printer> out;
    for (std::string_view line : lines(text)) {
        if (line.starts_with("printer ")) {
            // "printer Office is idle.  enabled since …", "printer Office now printing Office-3.  …",
            // "printer Office disabled since …"
            std::string_view rest = line.substr(8);
            const size_t space = rest.find(' ');
            Printer p;
            p.name = std::string(rest.substr(0, space));
            const std::string_view status = space == std::string_view::npos ? "" : rest.substr(space + 1);
            if (status.starts_with("disabled")) {
                p.state = "stopped";
                p.enabled = false;
            } else if (status.starts_with("now printing")) {
                p.state = "printing";
            } else {
                p.state = "idle";
            }
            out.push_back(std::move(p));
        } else if (!out.empty()) {
            const std::string_view t = trim(line);
            if (t.starts_with("Description:"))
                out.back().description = std::string(trim(t.substr(12)));
        }
    }
    return out;
}

std::string parse_default(std::string_view text) {
    constexpr std::string_view kPrefix = "system default destination: ";
    for (std::string_view line : lines(text))
        if (line.starts_with(kPrefix))
            return std::string(trim(line.substr(kPrefix.size())));
    return {};
}

std::vector<Job> parse_jobs(std::string_view text, const std::vector<Printer>& printers) {
    std::vector<Job> out;
    for (std::string_view line : lines(text)) {
        std::istringstream in{std::string(line)};
        Job j;
        if (!(in >> j.id >> j.user >> j.size))
            continue;
        // The longest printer name the id starts with, then "-N".
        for (const Printer& p : printers)
            if (j.id.size() > p.name.size() + 1 && j.id.starts_with(p.name + "-") && p.name.size() > j.printer.size())
                j.printer = p.name;
        if (!j.printer.empty())
            out.push_back(std::move(j));
    }
    return out;
}

std::vector<Option> parse_options(std::string_view text) {
    std::vector<Option> out;
    for (std::string_view line : lines(text)) {
        const size_t colon = line.find(": ");
        if (colon == std::string_view::npos)
            continue;
        const std::string_view head = line.substr(0, colon);
        const size_t slash = head.find('/');
        Option o;
        o.key = std::string(head.substr(0, slash));
        o.label = std::string(slash == std::string_view::npos ? head : head.substr(slash + 1));
        std::string_view rest = line.substr(colon + 2);
        while (!rest.empty()) {
            const size_t space = rest.find(' ');
            std::string_view word = rest.substr(0, space);
            rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
            if (word.empty())
                continue;
            if (word.front() == '*') {
                word.remove_prefix(1);
                o.current = std::string(word);
            }
            o.choices.emplace_back(word);
        }
        if (!o.key.empty() && !o.choices.empty())
            out.push_back(std::move(o));
    }
    return out;
}

namespace {

constexpr Paper kPapers[] = {
    {"A3", "iso_a3", "A3", 297, 420},
    {"A4", "iso_a4", "A4", 210, 297},
    {"A5", "iso_a5", "A5", 148, 210},
    {"A6", "iso_a6", "A6", 105, 148},
    {"B5", "jis_b5", "B5 (JIS)", 182, 257},
    {"ISOB5", "iso_b5", "B5", 176, 250},
    {"Letter", "na_letter", "US Letter", 215.9, 279.4},
    {"Legal", "na_legal", "US Legal", 215.9, 355.6},
    {"Executive", "na_executive", "Executive", 184.15, 266.7},
    {"Tabloid", "na_ledger", "Tabloid", 279.4, 431.8},
    {"Env10", "na_number-10", "Envelope #10", 104.775, 241.3},
    {"EnvDL", "iso_dl", "Envelope DL", 110, 220},
    {"EnvC5", "iso_c5", "Envelope C5", 162, 229},
    {"EnvMonarch", "na_monarch", "Envelope Monarch", 98.425, 190.5},
    {"4x6", "na_index-4x6", "4 × 6 in", 101.6, 152.4},
    {"Postcard", "jpn_hagaki", "Postcard", 100, 148},
};

} // namespace

const Paper* paper_by_ppd(std::string_view ppd) {
    for (const Paper& p : kPapers)
        if (p.ppd == ppd)
            return &p;
    return nullptr;
}

std::span<const Paper> papers() {
    return kPapers;
}

const Paper* paper_by_pwg(std::string_view pwg) {
    for (const Paper& p : kPapers)
        if (p.pwg == pwg)
            return &p;
    return nullptr;
}

std::optional<std::string> zero_based_ranges(std::string_view typed) {
    std::string out;
    auto number = [](std::string_view s, int& n) {
        s = trim(s);
        if (s.empty() || s.size() > 6)
            return false;
        n = 0;
        for (char c : s) {
            if (c < '0' || c > '9')
                return false;
            n = n * 10 + (c - '0');
        }
        return n >= 1;
    };
    bool any = false;
    while (true) {
        const size_t comma = typed.find(',');
        const std::string_view part = trim(typed.substr(0, comma));
        if (!part.empty()) {
            const size_t dash = part.find('-');
            int from = 0, to = 0;
            if (!number(part.substr(0, dash), from))
                return std::nullopt;
            to = from;
            if (dash != std::string_view::npos && !number(part.substr(dash + 1), to))
                return std::nullopt;
            if (to < from)
                return std::nullopt;
            if (any)
                out += ',';
            out += std::to_string(from - 1);
            if (to != from)
                out += '-' + std::to_string(to - 1);
            any = true;
        }
        if (comma == std::string_view::npos)
            break;
        typed.remove_prefix(comma + 1);
    }
    if (!any)
        return std::nullopt;
    return out;
}

std::vector<std::string> lp_args(const PrintJob& job) {
    std::vector<std::string> a;
    auto option = [&](const std::string& o) {
        a.push_back("-o");
        a.push_back(o);
    };
    if (!job.printer.empty())
        a.insert(a.end(), {"-d", job.printer});
    if (!job.title.empty())
        a.insert(a.end(), {"-t", job.title});
    if (job.copies > 1)
        a.insert(a.end(), {"-n", std::to_string(job.copies)});
    if (!job.ranges.empty())
        a.insert(a.end(), {"-P", job.ranges});
    if (job.page_set == "odd" || job.page_set == "even")
        option("page-set=" + job.page_set);
    if (!job.paper.empty())
        option("PageSize=" + job.paper);
    if (job.landscape)
        option("landscape");
    if (!job.duplex.empty())
        option("Duplex=" + job.duplex);
    if (!job.color_model.empty())
        option("ColorModel=" + job.color_model);
    if (job.copies > 1)
        option(job.collate ? "collate=true" : "collate=false");
    if (job.reverse)
        option("outputorder=reverse");
    if (job.number_up > 1)
        option("number-up=" + std::to_string(job.number_up));
    return a;
}

} // namespace atrium::printers
