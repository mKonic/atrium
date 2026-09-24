#pragma once
// The screenshot tool's arithmetic, without Qt: names, which window is
// under the pointer, and a selection on screen as pixels of its picture.

#include <ctime>
#include <string>
#include <vector>

namespace atrium::capture {

struct Box {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Box&) const = default;
};

// "Screenshot 2026-09-24 at 15.34.02.png", as macOS names them.
std::string file_name(const std::tm& when);

// The first box holding (x, y), boxes listed top first; -1 for none.
int topmost_at(const std::vector<Box>& boxes, int x, int y);

// `b` (logical, on a screen `screen_w` by `screen_h`) in the pixels of that
// screen's picture, `image_w` by `image_h`, kept inside it.
Box to_pixels(Box b, int screen_w, int screen_h, int image_w, int image_h);

// The rectangle between two corners, either way round.
Box between(int x0, int y0, int x1, int y1);

} // namespace atrium::capture
