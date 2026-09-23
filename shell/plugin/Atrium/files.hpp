#pragma once
// What the desktop shows for a file, as plain C++ so it is tested without
// Qt; desktop_files.cpp hands it to QML.

#include <functional>
#include <string>
#include <string_view>

namespace atrium::files {

// A theme icon name for a file by its suffix (any case).
std::string icon_for(std::string_view suffix, bool is_dir);

// Pictures the desktop shows as thumbnails.
bool is_image(std::string_view suffix);

// The name and icon a .desktop launcher gives itself, from its
// [Desktop Entry] group; empty when it gives none.
struct Launcher {
    std::string name;
    std::string icon;
};
Launcher parse_launcher(std::string_view text);

// `base`, or "base 2", "base 3"... whichever `taken` says is free first.
std::string free_name(const std::string& base, const std::function<bool(const std::string&)>& taken);

// Whether `name` can rename a file in place: not empty, no slash, not . or ..
bool valid_name(std::string_view name);

} // namespace atrium::files
