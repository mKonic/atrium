#pragma once
// Screenshots' pure parts: what area a shot covers, and a screen's frame put
// upright (a rotated screen's buffer is turned).
#include "util/box.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace atrium {

// The layout area a shot covers: `region` where it meets the screens, or all
// of them. Empty when nothing is shown there.
Box screenshot_area(const std::vector<Box>& outputs, const std::optional<Box>& region);

// A frame (ARGB, `stride` bytes a row) as the screen shows it: the buffer
// turned back by its output's `transform`. `width` x `height` comes out.
std::vector<uint32_t> screenshot_upright(const uint8_t* data, int buffer_width, int buffer_height, size_t stride,
                                         wl_output_transform transform, int* width, int* height);

} // namespace atrium
