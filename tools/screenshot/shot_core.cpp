#include "shot_core.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <unistd.h>

namespace atrium::shot {

std::optional<Box> parse_box(const std::string& text) {
    // strtol's rules, as grim's parse_box: "10,20 300x400".
    const char* s = text.c_str();
    char* end = nullptr;
    Box b;
    b.x = int(std::strtol(s, &end, 10));
    if (end == s || *end != ',')
        return std::nullopt;
    s = end + 1;
    b.y = int(std::strtol(s, &end, 10));
    if (end == s || *end != ' ')
        return std::nullopt;
    s = end + 1;
    b.width = int(std::strtol(s, &end, 10));
    if (end == s || *end != 'x')
        return std::nullopt;
    s = end + 1;
    b.height = int(std::strtol(s, &end, 10));
    if (end == s || *end != '\0' || b.width <= 0 || b.height <= 0)
        return std::nullopt;
    return b;
}

bool intersects(const Box& a, const Box& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

Box extents(const std::vector<Box>& boxes) {
    if (boxes.empty())
        return {};
    int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
    for (const Box& b : boxes) {
        x1 = std::min(x1, b.x);
        y1 = std::min(y1, b.y);
        x2 = std::max(x2, b.x + b.width);
        y2 = std::max(y2, b.y + b.height);
    }
    return {x1, y1, x2 - x1, y2 - y1};
}

double greatest_scale(const std::vector<double>& scales) {
    double s = 1;
    for (double v : scales)
        s = std::max(s, v);
    return s;
}

std::optional<FileType> file_type(const std::string& name) {
    if (name == "png")
        return FileType::Png;
    if (name == "ppm")
        return FileType::Ppm;
    if (name == "jpeg" || name == "jpg")
        return FileType::Jpeg;
    return std::nullopt;
}

const char* extension(FileType t) {
    switch (t) {
    case FileType::Png: return "png";
    case FileType::Ppm: return "ppm";
    case FileType::Jpeg: return "jpeg";
    }
    return "png";
}

std::string default_name(FileType t, std::time_t when) {
    std::tm tm{};
    localtime_r(&when, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y%m%d_%Hh%Mm%Ss_atrium.", &tm);
    return std::string(buf) + extension(t);
}

std::string pictures_dir(const char* grim_default_dir, const std::string& user_dirs, const char* home) {
    auto exists = [](const std::string& p) { return !p.empty() && access(p.c_str(), R_OK) == 0; };
    if (grim_default_dir && exists(grim_default_dir))
        return grim_default_dir;
    // XDG_PICTURES_DIR="$HOME/Pictures": the last line that says, $HOME expanded.
    std::string found;
    std::istringstream in(user_dirs);
    for (std::string line; std::getline(in, line);) {
        const size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos || line[start] == '#')
            continue;
        const std::string key = "XDG_PICTURES_DIR=";
        if (line.compare(start, key.size(), key) != 0)
            continue;
        std::string v = line.substr(start + key.size());
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        if (v.starts_with("$HOME"))
            v = std::string(home ? home : "") + v.substr(5);
        found = v;
    }
    return exists(found) ? found : ".";
}

} // namespace atrium::shot
