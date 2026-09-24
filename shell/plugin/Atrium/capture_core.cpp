#include "capture_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace atrium::capture {

std::string file_name(const std::tm& when) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "Screenshot %04d-%02d-%02d at %02d.%02d.%02d.png", when.tm_year + 1900,
                  when.tm_mon + 1, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec);
    return buf;
}

int topmost_at(const std::vector<Box>& boxes, int x, int y) {
    for (size_t i = 0; i < boxes.size(); ++i) {
        const Box& b = boxes[i];
        if (x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height)
            return int(i);
    }
    return -1;
}

Box to_pixels(Box b, int screen_w, int screen_h, int image_w, int image_h) {
    if (screen_w <= 0 || screen_h <= 0)
        return {};
    const double sx = double(image_w) / screen_w, sy = double(image_h) / screen_h;
    // Outward to whole pixels, so nothing selected is lost.
    int x0 = int(std::floor(b.x * sx)), y0 = int(std::floor(b.y * sy));
    int x1 = int(std::ceil((b.x + b.width) * sx)), y1 = int(std::ceil((b.y + b.height) * sy));
    x0 = std::clamp(x0, 0, image_w);
    y0 = std::clamp(y0, 0, image_h);
    x1 = std::clamp(x1, 0, image_w);
    y1 = std::clamp(y1, 0, image_h);
    return {x0, y0, x1 - x0, y1 - y0};
}

Box between(int x0, int y0, int x1, int y1) {
    return {std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)};
}

} // namespace atrium::capture
