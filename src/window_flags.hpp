#pragma once
#include <array>
#include <utility>

namespace atrium {

// How a window opens beyond where and how big, as Hyprland's window rules
// say it (none set: as it would anyway). Records, rules and what they come
// to for a window all carry these fields, so they are listed once here.
//
//   follow      go to its space when it opens there ("workspace N", not
//               "silent"; without it a window sent elsewhere opens quietly)
//   floating    stays out of the tiles on a tiled space ("float")
//   keep_above  over other windows ("pin", with sticky)
//   sticky      on every space
//   no_focus    opens without taking focus ("no_initial_focus")
template <class T>
auto window_flags(T& t) {
    return std::array{std::pair{"follow", &t.follow}, std::pair{"floating", &t.floating},
                      std::pair{"keep_above", &t.keep_above}, std::pair{"sticky", &t.sticky},
                      std::pair{"no_focus", &t.no_focus}};
}

} // namespace atrium
