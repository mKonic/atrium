#include "sysinfo_core.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace atrium::sysinfo {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// The last "[...]" of a pci.ids name, which is what people call the part.
std::string marketing(std::string_view name) {
    const size_t open = name.rfind('[');
    const size_t close = name.rfind(']');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open + 1)
        return std::string(name.substr(open + 1, close - open - 1));
    return std::string(name);
}

unsigned hex4(std::string_view s) {
    unsigned v = 0;
    for (char c : s.substr(0, 4)) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
        else return ~0u;
    }
    return v;
}

} // namespace

std::string os_release_value(std::string_view text, std::string_view key) {
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.size() > key.size() && line.substr(0, key.size()) == key && line[key.size()] == '=') {
            std::string_view v = line.substr(key.size() + 1);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
                v = v.substr(1, v.size() - 2);
            return std::string(v);
        }
    }
    return {};
}

std::string pci_device_name(std::istream& ids, unsigned vendor, unsigned device) {
    std::string line, vendor_name;
    bool in_vendor = false;
    while (std::getline(ids, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] != '\t') {
            if (line.rfind("C ", 0) == 0)
                break;  // device classes follow the vendors
            if (in_vendor)
                break;  // past our vendor without finding the device
            if (line.size() > 6 && hex4(line) == vendor) {
                in_vendor = true;
                vendor_name = std::string(trim(std::string_view(line).substr(4)));
            }
            continue;
        }
        if (!in_vendor || line.size() < 7 || line[1] == '\t')
            continue;  // subsystems
        if (hex4(std::string_view(line).substr(1)) == device) {
            std::string v = marketing(vendor_name);
            // "NVIDIA Corporation" → "NVIDIA"
            for (std::string_view suffix : {" Corporation", " Corp.", ", Inc.", " Inc."})
                if (v.size() > suffix.size() && v.ends_with(suffix))
                    v.resize(v.size() - suffix.size());
            return v + " " + marketing(trim(std::string_view(line).substr(6)));
        }
    }
    return {};
}

std::string tidy_cpu_name(std::string_view model) {
    std::string s(trim(model));
    for (std::string_view noise : {"(R)", "(TM)", "(tm)", " CPU", " Processor"}) {
        for (size_t at; (at = s.find(noise)) != std::string::npos;)
            s.erase(at, noise.size());
    }
    // "8-Core" and "@ 3.60GHz" say less than the name.
    if (size_t at = s.find(" @ "); at != std::string::npos)
        s.resize(at);
    if (size_t at = s.find("-Core"); at != std::string::npos) {
        size_t start = s.rfind(' ', at);
        s.erase(start == std::string::npos ? 0 : start, at + 5 - (start == std::string::npos ? 0 : start));
    }
    return std::string(trim(s));
}

std::string installed_memory(std::string_view udev_dmi) {
    unsigned long long bytes = 0;
    std::string type, speed;
    while (!udev_dmi.empty()) {
        const size_t nl = udev_dmi.find('\n');
        std::string_view line = trim(udev_dmi.substr(0, nl));
        udev_dmi = nl == std::string_view::npos ? std::string_view{} : udev_dmi.substr(nl + 1);
        if (!line.starts_with("E:MEMORY_DEVICE_"))
            continue;
        line.remove_prefix(2);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        const auto is = [&](std::string_view suffix) { return key.ends_with(suffix); };
        if (is("_VOLATILE_SIZE"))
            continue;
        if (is("_SIZE"))
            bytes += std::stoull(std::string(value));
        else if (is("_TYPE") && type.empty() && value != "Unknown")
            type = value;
        else if (is("_CONFIGURED_SPEED_MTS") && speed.empty() && value != "0")
            speed = value;
    }
    if (bytes == 0)
        return {};
    std::string out = std::to_string(bytes >> 30) + " GB";
    if (!type.empty())
        out += " " + type + (speed.empty() ? "" : "-" + speed);
    return out;
}

unsigned long long largest_bar(std::string_view resource) {
    unsigned long long best = 0;
    while (!resource.empty()) {
        const size_t nl = resource.find('\n');
        const std::string line(resource.substr(0, nl));
        resource = nl == std::string_view::npos ? std::string_view{} : resource.substr(nl + 1);
        unsigned long long start = 0, end = 0, flags = 0;
        if (std::sscanf(line.c_str(), "%llx %llx %llx", &start, &end, &flags) == 3 && end > start)
            best = std::max(best, end - start + 1);
    }
    return best;
}

std::string clock_time(double seconds) {
    const long total = seconds > 0 ? long(seconds) : 0;
    const long h = total / 3600, m = total / 60 % 60, s = total % 60;
    char buf[32];
    if (h)
        std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", h, m, s);
    else
        std::snprintf(buf, sizeof buf, "%ld:%02ld", m, s);
    return buf;
}

} // namespace atrium::sysinfo
