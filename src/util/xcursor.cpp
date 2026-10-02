#include "util/xcursor.hpp"

#include "util/log.hpp"

#include <cairo.h>
#include <dirent.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace atrium::xcursor {

namespace {

constexpr uint32_t kMagic = 0x72756358;  // "Xcur", little endian
constexpr uint32_t kImageType = 0xfffd0002;
constexpr uint32_t kMaxSide = 0x7fff;
constexpr uint32_t kMaxToc = 0x10000;

bool read_u32(FILE* f, uint32_t* out) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return false;
    *out = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    return true;
}

struct Toc {
    uint32_t type, subtype, position;
};

uint32_t dist(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

bool read_image(FILE* f, const Toc& toc, Image* out) {
    if (fseek(f, long(toc.position), SEEK_SET) != 0)
        return false;
    uint32_t header, type, subtype, version;
    if (!read_u32(f, &header) || !read_u32(f, &type) || !read_u32(f, &subtype) || !read_u32(f, &version))
        return false;
    if (type != toc.type || subtype != toc.subtype)
        return false;
    Image img;
    if (!read_u32(f, &img.width) || !read_u32(f, &img.height) || !read_u32(f, &img.hotspot_x) ||
        !read_u32(f, &img.hotspot_y) || !read_u32(f, &img.delay))
        return false;
    if (!img.width || !img.height || img.width > kMaxSide || img.height > kMaxSide || img.hotspot_x > img.width ||
        img.hotspot_y > img.height)
        return false;
    img.pixels.resize(size_t(img.width) * img.height);
    for (uint32_t& p : img.pixels)
        if (!read_u32(f, &p))
            return false;
    *out = std::move(img);
    return true;
}

// A plain arrow for when no theme is installed.
Cursor drawn_arrow(uint32_t size) {
    const int s = int(std::max<uint32_t>(size, 8));
    Image img;
    img.width = img.height = uint32_t(s);
    img.hotspot_x = img.hotspot_y = uint32_t(std::lround(s / 16.0));
    img.pixels.assign(size_t(s) * size_t(s), 0);
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(img.pixels.data()), CAIRO_FORMAT_ARGB32, s, s, s * 4);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, s, s);
    const double pts[][2] = {{0.06, 0.06}, {0.06, 0.78}, {0.24, 0.62}, {0.37, 0.92},
                             {0.48, 0.87}, {0.35, 0.58}, {0.58, 0.58}};
    cairo_move_to(cr, pts[0][0], pts[0][1]);
    for (const auto& p : pts)
        cairo_line_to(cr, p[0], p[1]);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_fill_preserve(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 1.5 / s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_stroke(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    Cursor c;
    c.images.push_back(std::move(img));
    return c;
}

std::string expand_home(std::string dir) {
    if (dir.starts_with('~'))
        if (const char* home = getenv("HOME"))
            dir = home + dir.substr(1);
    return dir;
}

} // namespace

std::vector<Image> read_file(FILE* f, uint32_t size) {
    uint32_t magic, header, version, ntoc;
    if (!read_u32(f, &magic) || magic != kMagic || !read_u32(f, &header) || !read_u32(f, &version) ||
        !read_u32(f, &ntoc) || ntoc > kMaxToc || header < 16)
        return {};
    if (header > 16 && fseek(f, long(header - 16), SEEK_CUR) != 0)
        return {};
    std::vector<Toc> tocs(ntoc);
    for (Toc& t : tocs)
        if (!read_u32(f, &t.type) || !read_u32(f, &t.subtype) || !read_u32(f, &t.position))
            return {};
    // The size nearest the one asked for; its images are the frames.
    uint32_t best = 0;
    for (const Toc& t : tocs)
        if (t.type == kImageType && (!best || dist(t.subtype, size) < dist(best, size)))
            best = t.subtype;
    std::vector<Image> out;
    for (const Toc& t : tocs) {
        if (t.type != kImageType || t.subtype != best)
            continue;
        Image img;
        if (!read_image(f, t, &img))
            return {};
        out.push_back(std::move(img));
    }
    return out;
}

std::vector<std::string> inherits(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream in{std::string(text)};
    std::string line;
    const auto sep = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';' || c == ','; };
    while (std::getline(in, line)) {
        if (!line.starts_with("Inherits"))
            continue;
        size_t i = 8;
        while (i < line.size() && line[i] == ' ')
            ++i;
        if (i >= line.size() || line[i] != '=')
            continue;
        ++i;
        while (i < line.size()) {
            while (i < line.size() && sep(line[i]))
                ++i;
            const size_t start = i;
            while (i < line.size() && !sep(line[i]))
                ++i;
            if (i > start)
                out.emplace_back(line.substr(start, i - start));
        }
        break;
    }
    return out;
}

std::vector<std::string> search_path() {
    std::string path;
    if (const char* env = getenv("XCURSOR_PATH")) {
        path = env;
    } else {
        const char* data = getenv("XDG_DATA_HOME");
        path = std::string(data && data[0] == '/' ? data : "~/.local/share") +
               "/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps:~/.cursors:/usr/share/cursors/xorg-x11:"
               "/usr/X11R6/lib/X11/icons";
    }
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos)
            end = path.size();
        if (end > start)
            out.push_back(expand_home(path.substr(start, end - start)));
        start = end + 1;
    }
    return out;
}

std::string_view legacy_name(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
        {"default", "left_ptr"},          {"text", "xterm"},
        {"pointer", "hand1"},             {"wait", "watch"},
        {"all-scroll", "grabbing"},       {"sw-resize", "bottom_left_corner"},
        {"se-resize", "bottom_right_corner"}, {"s-resize", "bottom_side"},
        {"w-resize", "left_side"},        {"e-resize", "right_side"},
        {"nw-resize", "top_left_corner"}, {"ne-resize", "top_right_corner"},
        {"n-resize", "top_side"},
    };
    for (const auto& [css, x11] : kNames)
        if (css == name)
            return x11;
    return {};
}

Theme::Theme(const char* name, uint32_t size, const std::vector<std::string>& path) {
    std::vector<std::string> visited;
    load(name && *name ? name : "default", size, path, visited);
    if (cursors_.empty()) {
        alog(Log::Info, "xcursor: no cursor theme '%s'; drawing an arrow", name ? name : "default");
        cursors_.emplace("left_ptr", drawn_arrow(size));
        fallback_ = true;
    }
}

void Theme::load(const std::string& name, uint32_t size, const std::vector<std::string>& path,
                 std::vector<std::string>& visited) {
    visited.push_back(name);
    std::vector<std::string> parents;
    bool have_parents = false;
    for (const std::string& base : path) {
        const std::string dir = base + "/" + name;
        const std::string cursors = dir + "/cursors";
        if (DIR* d = opendir(cursors.c_str())) {
            while (dirent* ent = readdir(d)) {
                if (ent->d_type != DT_UNKNOWN && ent->d_type != DT_REG && ent->d_type != DT_LNK)
                    continue;
                // The first theme (and directory) to have a cursor wins.
                if (cursors_.contains(std::string_view(ent->d_name)))
                    continue;
                FILE* f = fopen((cursors + "/" + ent->d_name).c_str(), "rbe");
                if (!f)
                    continue;
                std::vector<Image> images = read_file(f, size);
                fclose(f);
                if (!images.empty())
                    cursors_.emplace(ent->d_name, Cursor{std::move(images)});
            }
            closedir(d);
        }
        if (!have_parents) {
            std::ifstream index(dir + "/index.theme");
            if (index) {
                std::stringstream text;
                text << index.rdbuf();
                parents = inherits(text.str());
                have_parents = true;
            }
        }
    }
    for (const std::string& p : parents)
        if (std::ranges::find(visited, p) == visited.end())
            load(p, size, path, visited);
}

const Cursor* Theme::get(std::string_view name) const {
    if (auto it = cursors_.find(name); it != cursors_.end())
        return &it->second;
    if (const std::string_view legacy = legacy_name(name); !legacy.empty())
        if (auto it = cursors_.find(legacy); it != cursors_.end())
            return &it->second;
    return fallback_ ? &cursors_.begin()->second : nullptr;
}

const Cursor* Manager::get(std::string_view name, float scale) {
    auto it = std::ranges::find(scaled_, scale, &decltype(scaled_)::value_type::first);
    if (it == scaled_.end()) {
        scaled_.emplace_back(scale, std::make_unique<Theme>(name_.empty() ? nullptr : name_.c_str(),
                                                            uint32_t(std::lround(size_ * scale))));
        it = scaled_.end() - 1;
    }
    return it->second->get(name);
}

} // namespace atrium::xcursor
