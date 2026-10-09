#pragma once
// atrium-screenshot's plain parts, as grim has them (box.c, output-layout.c,
// main.c): the -g geometry, which screens a region takes in, how big the
// picture is, and the file it goes to when none is named.

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace atrium::shot {

struct Box {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Box&) const = default;
};

// "X,Y WxH", as slurp prints it.
std::optional<Box> parse_box(const std::string& text);
bool intersects(const Box& a, const Box& b);
// The box around all of them.
Box extents(const std::vector<Box>& boxes);

// The scale a picture is taken at, unless -s says: the greatest of the
// screens it covers (a 2x screen beside a 1x one stays sharp).
double greatest_scale(const std::vector<double>& scales);

enum class FileType { Png, Ppm, Jpeg };
std::optional<FileType> file_type(const std::string& name);
const char* extension(FileType t);
// "20261009_14h03m22s_atrium.png", as grim names them.
std::string default_name(FileType t, std::time_t when);
// Where unnamed pictures go: $GRIM_DEFAULT_DIR, else XDG_PICTURES_DIR from
// user-dirs.dirs (its text given), else here.
std::string pictures_dir(const char* grim_default_dir, const std::string& user_dirs, const char* home);

} // namespace atrium::shot
