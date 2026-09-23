#pragma once
#include <cstdint>
#include <string>

namespace atrium {

// Where an app's window was when it last closed (kept on its registry record): the floating box (relative
// to its output's top-left, title bar included) and whether it was maximized
// or snapped. Its first window comes back there on the next launch.
struct Placement {
    std::string output;
    int x = 0, y = 0, width = 0, height = 0;
    bool maximized = false;
    uint32_t snapped = 0;
    bool operator==(const Placement&) const = default;
};

} // namespace atrium
