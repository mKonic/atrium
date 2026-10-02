#pragma once
// Xcursor themes: the cursor files of a theme and those it inherits from,
// at the size nearest the one asked for. The file format and search rules
// are libXcursor's (by way of wayland-cursor and wlroots' xcursor/, MIT).
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace atrium::xcursor {

struct Image {
    uint32_t width = 0, height = 0;
    uint32_t hotspot_x = 0, hotspot_y = 0;
    uint32_t delay = 0;            // ms before the next frame
    std::vector<uint32_t> pixels;  // premultiplied ARGB8888, width * height
};

// A cursor's frames (one for a still cursor).
struct Cursor {
    std::vector<Image> images;
};

// The images of one Xcursor file nearest `size`; empty if it isn't one.
std::vector<Image> read_file(FILE* file, uint32_t size);
// The themes an index.theme's "Inherits=" names, in order.
std::vector<std::string> inherits(std::string_view index_theme);
// Where themes are looked for: $XCURSOR_PATH, else the XDG data home's
// icons and the usual system places ("~" expanded).
std::vector<std::string> search_path();
// The X11 name of a CSS cursor name ("default" → "left_ptr"), "" if none.
std::string_view legacy_name(std::string_view name);

class Theme {
public:
    // `name` (null: "default") at `size`; a drawn arrow if it has no cursors.
    Theme(const char* name, uint32_t size, const std::vector<std::string>& path = search_path());
    // By name, then legacy name; null if neither (the drawn arrow answers
    // every name).
    const Cursor* get(std::string_view name) const;
    size_t count() const { return cursors_.size(); }
    bool fallback() const { return fallback_; }

private:
    void load(const std::string& name, uint32_t size, const std::vector<std::string>& path,
              std::vector<std::string>& visited);
    std::map<std::string, Cursor, std::less<>> cursors_;
    bool fallback_ = false;
};

// A theme at a base size, loaded per scale on first use.
class Manager {
public:
    Manager(const char* theme, uint32_t size) : name_(theme ? theme : ""), size_(size) {}
    const Cursor* get(std::string_view name, float scale);

private:
    std::string name_;
    uint32_t size_;
    std::vector<std::pair<float, std::unique_ptr<Theme>>> scaled_;
};

} // namespace atrium::xcursor
