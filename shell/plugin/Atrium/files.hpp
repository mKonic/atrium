#pragma once
// What the desktop shows for a file, as plain C++ so it is tested without
// Qt; desktop_files.cpp hands it to QML.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

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

// `name`, or "name 2.ext", "name 3.ext"... whichever `taken` says is free
// first: the number goes before the suffix, as Finder puts it.
std::string free_file_name(const std::string& name, const std::function<bool(const std::string&)>& taken);

// Whether `name` can rename a file in place: not empty, no slash, not . or ..
bool valid_name(std::string_view name);

// What a path typed into a file picker means: "~" is home, a relative path
// is under `current`, and "." and ".." steps are taken. Always absolute.
std::string resolve_typed(std::string_view typed, const std::string& current, const std::string& home);

// Whether a file's name ends in one of `suffixes` ("png", any case); no
// suffixes lets everything through.
bool has_suffix(std::string_view name, const std::vector<std::string>& suffixes);

// Whether `name` matches a shell glob ("*.png", "IMG_????.jpg", "[ab]*"),
// in any case.
bool glob_match(std::string_view name, std::string_view pattern);

// The suffixes a Qt-style name filter lists: "Pictures (*.png *.jpg)" gives
// png and jpg; a filter of "*" or "*.*" gives none (everything).
std::vector<std::string> filter_suffixes(std::string_view filter);

} // namespace atrium::files
