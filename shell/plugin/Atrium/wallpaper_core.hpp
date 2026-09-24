#pragma once
// Where other desktops keep their wallpaper, read out of their files, so the
// welcome can offer to bring it along. Each returns the picture's path (with
// ~ expanded against `home`), or "" when the file names none.

#include <string>
#include <string_view>

namespace atrium::wallpaper {

// hyprpaper.conf: `wallpaper = MONITOR,PATH`, a `wallpaper { path = PATH }`
// block, or else the first `preload = PATH`.
std::string from_hyprpaper(std::string_view conf, std::string_view home);

// Plasma's plasma-org.kde.plasma.desktop-appletsrc: the first `Image=` of an
// org.kde.image wallpaper (a file:// URL or a path).
std::string from_plasma(std::string_view conf, std::string_view home);

// waypaper's config.ini: `wallpaper = PATH`.
std::string from_waypaper(std::string_view conf, std::string_view home);

// A path as written in those files: "~/x", "file:///x" (with %20 and the
// like decoded), or a plain one. Surrounding quotes and spaces go.
std::string clean_path(std::string_view raw, std::string_view home);

} // namespace atrium::wallpaper
