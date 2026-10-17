#pragma once
// What the desktop shows for a file, as plain C++ so it is tested without
// Qt; desktop_files.cpp hands it to QML.

#include <functional>
#include <map>
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

// The desktop's grid: columns of icons from the top left, as Windows and
// Plasma put them.
struct Cell {
    int col = 0, row = 0;
    bool operator==(const Cell&) const = default;
    auto operator<=>(const Cell&) const = default;
};

// Where `names` go on a grid `cols` by `rows`: a name keeps its `saved`
// cell when that is on the grid and still free; the rest, in order, take
// the free cells down the columns. Past a full grid, more columns.
std::vector<Cell> place(const std::vector<std::string>& names, const std::map<std::string, Cell>& saved, int cols,
                        int rows);

// The icon an arrow key reaches from `from` (dx, dy one of -1, 0, 1): the
// nearest one that way, a straight line winning a tie; -1 for none. With
// nothing current (-1), the first down the columns.
int nearest(const std::vector<Cell>& cells, int from, int dx, int dy);

// The icons in reading order on the desktop: down each column, then the
// next column. What Shift+click selects a run of.
std::vector<int> grid_order(const std::vector<Cell>& cells);

// Typing a name's start selects it: the first name, in `order`, starting
// with `typed` (any case). One letter again goes to the next with it,
// after `from`. -1 when none matches.
int type_ahead(const std::vector<std::string>& names, const std::vector<int>& order, int from,
               std::string_view typed);

} // namespace atrium::files
