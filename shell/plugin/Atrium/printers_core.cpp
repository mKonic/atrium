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

} // namespace atrium::printers
