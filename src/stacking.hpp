#pragma once
// Stacking rules, free of any compositor state so they can be tested on
// their own.

#include <cstddef>
#include <span>

namespace atrium::stacking {

struct Window {
    bool fullscreen = false;
    bool shown = false;    // mapped, not minimized
    bool managed = true;   // not an override-redirect X11 window
    const void* output = nullptr;
    const void* space = nullptr;
};

// A fullscreen window is over everything (the panels too) only while it is
// the front window of its screen and space, as on Windows, macOS and KDE.
// `mru` is most recently used first; returns the index of the window in
// front of mru[i], or -1 when nothing is.
int in_front_of(std::span<const Window> mru, std::size_t i);

} // namespace atrium::stacking
