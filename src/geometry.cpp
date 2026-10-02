#include "geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace atrium::geometry {

Box place(int w, int h, const Box& area, const Box* parent,
              std::span<const Box> others, int step) {
    Box g{0, 0, std::min(w, area.width), std::min(h, area.height)};

    if (parent) {
        g.x = parent->x + (parent->width - g.width) / 2;
        g.y = parent->y + (parent->height - g.height) / 2;
    } else {
        g.x = area.x + (area.width - g.width) / 2;
        g.y = area.y + (area.height - g.height) / 2;
        const size_t limit = others.size() + 1;
        for (size_t tries = 0; tries < limit && step > 0; ++tries) {
            const bool taken = std::ranges::any_of(others, [&](const Box& o) {
                return std::abs(o.x - g.x) < step && std::abs(o.y - g.y) < step;
            });
            if (!taken)
                break;
            g.x += step;
            g.y += step;
            if (g.x + g.width > area.x + area.width || g.y + g.height > area.y + area.height) {
                g.x = area.x;
                g.y = area.y;
            }
        }
    }

    g.x = std::clamp(g.x, area.x, std::max(area.x, area.x + area.width - g.width));
    g.y = std::clamp(g.y, area.y, std::max(area.y, area.y + area.height - g.height));
    return g;
}

void snap(int& x, int& y, int w, int h, const Box& area, int distance) {
    if (std::abs(x - area.x) < distance)
        x = area.x;
    else if (std::abs(x + w - (area.x + area.width)) < distance)
        x = area.x + area.width - w;
    if (std::abs(y - area.y) < distance)
        y = area.y;
    else if (std::abs(y + h - (area.y + area.height)) < distance)
        y = area.y + area.height - h;
}

Box resize(const Box& start, uint32_t edges, int dx, int dy) {
    Box b = start;
    if (edges & EDGE_LEFT) {
        b.width = std::max(start.width - dx, 1);
        b.x = start.x + start.width - b.width;
    } else if (edges & EDGE_RIGHT) {
        b.width = std::max(start.width + dx, 1);
    }
    if (edges & EDGE_TOP) {
        b.height = std::max(start.height - dy, 1);
        b.y = start.y + start.height - b.height;
    } else if (edges & EDGE_BOTTOM) {
        b.height = std::max(start.height + dy, 1);
    }
    return b;
}

Box clamp_to_hints(Box box, const Box& min, const Box& max) {
    box.width = std::max({box.width, min.width, 1});
    box.height = std::max({box.height, min.height, 1});
    if (max.width > 0)
        box.width = std::min(box.width, std::max(max.width, 1));
    if (max.height > 0)
        box.height = std::min(box.height, std::max(max.height, 1));
    return box;
}

uint32_t nearest_corner(const Box& window, double cx, double cy) {
    return (cx < window.x + window.width / 2.0 ? EDGE_LEFT : EDGE_RIGHT) |
           (cy < window.y + window.height / 2.0 ? EDGE_TOP : EDGE_BOTTOM);
}

uint32_t snap_zone(const Box& area, double cx, double cy, int edge, int corner) {
    const bool left = cx < area.x + edge;
    const bool right = cx >= area.x + area.width - edge;
    const bool top = cy < area.y + edge;
    const bool bottom = cy >= area.y + area.height - edge;
    const bool near_top = cy < area.y + corner;
    const bool near_bottom = cy >= area.y + area.height - corner;
    const bool near_left = cx < area.x + corner;
    const bool near_right = cx >= area.x + area.width - corner;

    if (left || right) {
        const uint32_t side = left ? EDGE_LEFT : EDGE_RIGHT;
        if (near_top)
            return side | EDGE_TOP;
        if (near_bottom)
            return side | EDGE_BOTTOM;
        return side;
    }
    if (top) {
        if (near_left)
            return EDGE_LEFT | EDGE_TOP;
        if (near_right)
            return EDGE_RIGHT | EDGE_TOP;
        return EDGE_TOP;
    }
    if (bottom) {
        if (near_left)
            return EDGE_LEFT | EDGE_BOTTOM;
        if (near_right)
            return EDGE_RIGHT | EDGE_BOTTOM;
    }
    return 0;
}

Box snap_box(const Box& area, uint32_t zone, int gap) {
    if (zone == EDGE_TOP || !zone)
        return area;
    Box inner{area.x + gap, area.y + gap, area.width - 2 * gap, area.height - 2 * gap};
    Box b = inner;
    const int half_w = (inner.width - gap) / 2;
    const int half_h = (inner.height - gap) / 2;
    if (zone & (EDGE_LEFT | EDGE_RIGHT)) {
        b.width = half_w;
        if (zone & EDGE_RIGHT)
            b.x = inner.x + inner.width - half_w;
    }
    if ((zone & (EDGE_LEFT | EDGE_RIGHT)) && (zone & (EDGE_TOP | EDGE_BOTTOM))) {
        b.height = half_h;
        if (zone & EDGE_BOTTOM)
            b.y = inner.y + inner.height - half_h;
    }
    return b;
}

namespace {

// The part of `area` between fractions x0..x1, y0..y1, with `gap` at the
// area's edges and half of it where two parts meet.
Box fraction(const Box& area, double x0, double y0, double x1, double y1, int gap) {
    auto edge = [gap](double f) { return f <= 0.0001 || f >= 0.9999 ? gap : gap / 2; };
    const int left = area.x + int(std::lround(area.width * x0)) + edge(x0);
    const int right = area.x + int(std::lround(area.width * x1)) - edge(x1);
    const int top = area.y + int(std::lround(area.height * y0)) + edge(y0);
    const int bottom = area.y + int(std::lround(area.height * y1)) - edge(y1);
    return {left, top, std::max(1, right - left), std::max(1, bottom - top)};
}

bool same_box(const Box& a, const Box& b) {
    return std::abs(a.x - b.x) <= 2 && std::abs(a.y - b.y) <= 2 && std::abs(a.width - b.width) <= 2 &&
           std::abs(a.height - b.height) <= 2;
}

Box centered(const Box& area, int w, int h) {
    w = std::min(w, area.width);
    h = std::min(h, area.height);
    return {area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h};
}

constexpr std::string_view kPlaceNames[] = {
    "left-half", "right-half", "top-half", "bottom-half", "center-half",
    "top-left", "top-right", "bottom-left", "bottom-right",
    "first-third", "center-third", "last-third", "first-two-thirds", "center-two-thirds", "last-two-thirds",
    "first-fourth", "second-fourth", "third-fourth", "last-fourth", "first-three-fourths", "last-three-fourths",
    "top-left-sixth", "top-center-sixth", "top-right-sixth", "bottom-left-sixth", "bottom-center-sixth",
    "bottom-right-sixth",
    "almost-maximize", "maximize-height", "maximize-width", "larger", "smaller", "center",
    "move-left", "move-right", "move-up", "move-down",
};

} // namespace

std::span<const std::string_view> place_names() {
    return kPlaceNames;
}

std::optional<Box> named_place(std::string_view name, const Box& area, const Box& cur, int gap, bool again) {
    auto f = [&](double x0, double y0, double x1, double y1) { return fraction(area, x0, y0, x1, y1, gap); };
    // A half pressed again: two thirds, then one third, then back.
    auto cycle = [&](bool left) {
        const Box half = left ? f(0, 0, 0.5, 1) : f(0.5, 0, 1, 1);
        const Box two = left ? f(0, 0, 2.0 / 3, 1) : f(1.0 / 3, 0, 1, 1);
        const Box third = left ? f(0, 0, 1.0 / 3, 1) : f(2.0 / 3, 0, 1, 1);
        if (!again)
            return half;
        return same_box(cur, half) ? two : same_box(cur, two) ? third : half;
    };
    const Box inner{area.x + gap, area.y + gap, area.width - 2 * gap, area.height - 2 * gap};
    if (name == "left-half") return cycle(true);
    if (name == "right-half") return cycle(false);
    if (name == "top-half") return f(0, 0, 1, 0.5);
    if (name == "bottom-half") return f(0, 0.5, 1, 1);
    if (name == "center-half") return f(0.25, 0, 0.75, 1);
    if (name == "top-left") return f(0, 0, 0.5, 0.5);
    if (name == "top-right") return f(0.5, 0, 1, 0.5);
    if (name == "bottom-left") return f(0, 0.5, 0.5, 1);
    if (name == "bottom-right") return f(0.5, 0.5, 1, 1);
    if (name == "first-third") return f(0, 0, 1.0 / 3, 1);
    if (name == "center-third") return f(1.0 / 3, 0, 2.0 / 3, 1);
    if (name == "last-third") return f(2.0 / 3, 0, 1, 1);
    if (name == "first-two-thirds") return f(0, 0, 2.0 / 3, 1);
    if (name == "center-two-thirds") return f(1.0 / 6, 0, 5.0 / 6, 1);
    if (name == "last-two-thirds") return f(1.0 / 3, 0, 1, 1);
    if (name == "first-fourth") return f(0, 0, 0.25, 1);
    if (name == "second-fourth") return f(0.25, 0, 0.5, 1);
    if (name == "third-fourth") return f(0.5, 0, 0.75, 1);
    if (name == "last-fourth") return f(0.75, 0, 1, 1);
    if (name == "first-three-fourths") return f(0, 0, 0.75, 1);
    if (name == "last-three-fourths") return f(0.25, 0, 1, 1);
    if (name.ends_with("-sixth")) {
        const double y0 = name.starts_with("top") ? 0 : 0.5;
        const double x0 = name.find("left") != std::string_view::npos    ? 0
                          : name.find("center") != std::string_view::npos ? 1.0 / 3
                          : name.find("right") != std::string_view::npos  ? 2.0 / 3
                                                                          : -1;
        if (x0 < 0)
            return std::nullopt;
        return f(x0, y0, x0 + 1.0 / 3, y0 + 0.5);
    }
    if (name == "almost-maximize")
        return centered(area, int(area.width * 0.9), int(area.height * 0.9));
    if (name == "maximize-height") return Box{cur.x, inner.y, cur.width, inner.height};
    if (name == "maximize-width") return Box{inner.x, cur.y, inner.width, cur.height};
    if (name == "larger" || name == "smaller") {
        const int step = std::max(area.width, area.height) / 20 * (name == "larger" ? 1 : -1);
        Box b{cur.x - step, cur.y - step, cur.width + 2 * step, cur.height + 2 * step};
        if (b.width < 200 || b.height < 150)
            return cur;
        return fit_into(b, inner);
    }
    if (name == "center") return centered(area, cur.width, cur.height);
    if (name == "move-left") return fit_into({inner.x, cur.y, cur.width, cur.height}, inner);
    if (name == "move-right") return fit_into({inner.x + inner.width - cur.width, cur.y, cur.width, cur.height}, inner);
    if (name == "move-up") return fit_into({cur.x, inner.y, cur.width, cur.height}, inner);
    if (name == "move-down") return fit_into({cur.x, inner.y + inner.height - cur.height, cur.width, cur.height}, inner);
    if (name.starts_with("size:")) {
        double w = 0, h = 0;
        if (std::sscanf(std::string(name.substr(5)).c_str(), "%lfx%lf", &w, &h) != 2 || w <= 0 || h <= 0 || w > 1 ||
            h > 1)
            return std::nullopt;
        return centered(inner, int(inner.width * w), int(inner.height * h));
    }
    return std::nullopt;
}

Box fit_into(Box box, const Box& area) {
    box.width = std::clamp(box.width, 1, std::max(1, area.width));
    box.height = std::clamp(box.height, 1, std::max(1, area.height));
    box.x = std::clamp(box.x, area.x, area.x + area.width - box.width);
    box.y = std::clamp(box.y, area.y, area.y + area.height - box.height);
    return box;
}

namespace {

struct Candidate {
    std::vector<Box> boxes;
    double coverage = -1;  // total scaled window area
};

// Lay `order` out in `rows` rows of about equal count.
Candidate try_rows(std::span<const Box> windows, const std::vector<size_t>& order,
                   const Box& area, int gap, int label, int rows) {
    const size_t n = order.size();
    std::vector<std::vector<size_t>> grid(static_cast<size_t>(rows));
    for (size_t i = 0; i < n; ++i)
        grid[i * size_t(rows) / n].push_back(order[i]);
    // Inside a row, left to right.
    for (auto& row : grid)
        std::ranges::sort(row, [&](size_t a, size_t b) {
            return windows[a].x * 2 + windows[a].width < windows[b].x * 2 + windows[b].width;
        });

    // One height for every row: the tallest that lets each row fit the width
    // and all rows fit the height.
    double h = double(area.height - (rows - 1) * gap - rows * label) / rows;
    for (const auto& row : grid) {
        double aspect = 0;
        for (size_t i : row)
            aspect += double(windows[i].width) / std::max(windows[i].height, 1);
        const double fit = double(area.width - int(row.size() - 1) * gap) / aspect;
        h = std::min(h, fit);
    }
    Candidate c;
    if (h <= 1)
        return c;
    c.boxes.resize(n);
    c.coverage = 0;

    // Windows shorter than h keep their size; everything is centered.
    const double used_h = rows * (h + label) + (rows - 1) * gap;
    double y = area.y + (area.height - used_h) / 2;
    for (const auto& row : grid) {
        std::vector<Box> scaled;
        double row_w = double(row.size() - 1) * gap;
        for (size_t i : row) {
            const Box& w = windows[i];
            const double s = std::min(1.0, h / std::max(w.height, 1));
            scaled.push_back({0, 0, std::max(1, int(std::lround(w.width * s))),
                              std::max(1, int(std::lround(w.height * s)))});
            row_w += scaled.back().width;
        }
        double x = area.x + (area.width - row_w) / 2;
        for (size_t k = 0; k < row.size(); ++k) {
            Box b = scaled[k];
            b.x = int(std::lround(x));
            b.y = int(std::lround(y + (h - b.height) / 2));
            c.boxes[row[k]] = b;
            c.coverage += double(b.width) * b.height;
            x += b.width + gap;
        }
        y += h + label + gap;
    }
    return c;
}

} // namespace

std::vector<Box> overview_layout(std::span<const Box> windows, const Box& area,
                                     int gap, int label) {
    const size_t n = windows.size();
    if (!n)
        return {};
    // Top to bottom by center, so rows keep the windows' vertical order.
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = i;
    std::ranges::stable_sort(order, [&](size_t a, size_t b) {
        return windows[a].y * 2 + windows[a].height < windows[b].y * 2 + windows[b].height;
    });

    Candidate best;
    for (int rows = 1; rows <= int(n); ++rows) {
        Candidate c = try_rows(windows, order, area, gap, label, rows);
        // A new row has to earn its place: a small gain is not worth
        // shrinking every window.
        if (c.coverage > best.coverage * 1.05)
            best = std::move(c);
    }
    if (best.boxes.empty())  // area too small for anything: stack them in the middle
        for (size_t i = 0; i < n; ++i)
            best.boxes.push_back({area.x + area.width / 2, area.y + area.height / 2, 1, 1});
    return best.boxes;
}

Box secret_frame(const Box& area, int percent) {
    percent = std::clamp(percent, 0, 40);
    // The same margin on every side, measured on the shorter one.
    const int m = int(std::lround(std::min(area.width, area.height) * percent / 100.0));
    return {area.x + m, area.y + m, std::max(1, area.width - 2 * m), std::max(1, area.height - 2 * m)};
}

std::vector<Box> dwindle(size_t count, const Box& area, int gap) {
    std::vector<Box> out;
    Box rest = area;
    for (size_t i = 0; i < count; ++i) {
        if (i + 1 == count) {
            out.push_back(rest);
            break;
        }
        Box a = rest, b = rest;
        if (rest.width >= rest.height) {
            a.width = std::max(1, (rest.width - gap) / 2);
            b.x = rest.x + a.width + gap;
            b.width = std::max(1, rest.width - a.width - gap);
        } else {
            a.height = std::max(1, (rest.height - gap) / 2);
            b.y = rest.y + a.height + gap;
            b.height = std::max(1, rest.height - a.height - gap);
        }
        out.push_back(a);
        rest = b;
    }
    return out;
}

int neighbor(const Box& from, std::span<const Box> others, uint32_t direction) {
    const double fx = from.x + from.width / 2.0, fy = from.y + from.height / 2.0;
    int best = -1;
    double best_cost = 0;
    for (size_t i = 0; i < others.size(); ++i) {
        const Box& o = others[i];
        const double dx = o.x + o.width / 2.0 - fx, dy = o.y + o.height / 2.0 - fy;
        double ahead = 0, side = 0;
        switch (direction) {
        case EDGE_LEFT: ahead = -dx; side = dy; break;
        case EDGE_RIGHT: ahead = dx; side = dy; break;
        case EDGE_TOP: ahead = -dy; side = dx; break;
        case EDGE_BOTTOM: ahead = dy; side = dx; break;
        default: return -1;
        }
        if (ahead <= 0)
            continue;
        const double cost = ahead + 2 * std::abs(side);
        if (best < 0 || cost < best_cost) {
            best = int(i);
            best_cost = cost;
        }
    }
    return best;
}

std::optional<ClippedPiece> clip_to_frame(const Box& box, const FBox& src, int buffer_w, int buffer_h,
                                          int frame_w, int frame_h) {
    const int x0 = std::max(box.x, 0), y0 = std::max(box.y, 0);
    const int x1 = std::min(box.x + box.width, frame_w), y1 = std::min(box.y + box.height, frame_h);
    if (x1 <= x0 || y1 <= y0 || box.width <= 0 || box.height <= 0)
        return std::nullopt;
    FBox from = src;
    if (fbox_empty(&from))
        from = {0, 0, double(buffer_w), double(buffer_h)};
    const double kx = from.width / box.width, ky = from.height / box.height;
    return ClippedPiece{{x0, y0, x1 - x0, y1 - y0},
                        {from.x + (x0 - box.x) * kx, from.y + (y0 - box.y) * ky, (x1 - x0) * kx, (y1 - y0) * ky}};
}

} // namespace atrium::geometry
