#include "output_layout.hpp"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>

namespace atrium {

namespace {

Box box_of(const backend::Output* o, int x, int y) {
    Box b{x, y, 0, 0};
    o->effective_resolution(&b.width, &b.height);
    return b;
}

bool contains_point(const Box& b, double x, double y) {
    return b.width > 0 && b.height > 0 && x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

} // namespace

OutputLayout::~OutputLayout() = default;

OutputLayout::Entry* OutputLayout::find(const backend::Output* o) {
    auto it = std::ranges::find_if(entries_, [o](const auto& e) { return e->output == o; });
    return it == entries_.end() ? nullptr : it->get();
}

const OutputLayout::Entry* OutputLayout::find(const backend::Output* o) const {
    return const_cast<OutputLayout*>(this)->find(o);
}

OutputLayout::Entry& OutputLayout::insert(backend::Output* o) {
    if (Entry* e = find(o))
        return *e;
    auto e = std::make_unique<Entry>();
    e->output = o;
    // A new mode, scale or transform resizes it, and moves the automatic ones.
    e->commit = o->events.commit.connect([this](const backend::OutputState& s) {
        if (s.committed & (backend::OutputState::ModeField | backend::OutputState::Scale |
                           backend::OutputState::Transform | backend::OutputState::Enabled))
            reconfigure();
    });
    e->destroy = o->events.destroy.connect([this, o] { remove(o); });
    entries_.push_back(std::move(e));
    return *entries_.back();
}

void OutputLayout::add(backend::Output* o, int x, int y) {
    Entry& e = insert(o);
    e.automatic = false;
    e.x = x;
    e.y = y;
    reconfigure();
}

void OutputLayout::add_auto(backend::Output* o) {
    insert(o).automatic = true;
    reconfigure();
}

void OutputLayout::remove(backend::Output* o) {
    if (std::erase_if(entries_, [o](const auto& e) { return e->output == o; }))
        reconfigure();
}

std::vector<backend::Output*> OutputLayout::outputs() const {
    std::vector<backend::Output*> out;
    for (const auto& e : entries_)
        out.push_back(e->output);
    return out;
}

bool OutputLayout::contains(const backend::Output* o) const {
    return find(o) != nullptr;
}

Box OutputLayout::box(const backend::Output* o) const {
    const Entry* e = find(o);
    return e ? box_of(o, e->x, e->y) : Box{};
}

Box OutputLayout::extents() const {
    if (entries_.empty())
        return {};
    int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
    for (const auto& e : entries_) {
        const Box b = box_of(e->output, e->x, e->y);
        x1 = std::min(x1, b.x);
        y1 = std::min(y1, b.y);
        x2 = std::max(x2, b.x + b.width);
        y2 = std::max(y2, b.y + b.height);
    }
    return {x1, y1, x2 - x1, y2 - y1};
}

backend::Output* OutputLayout::output_at(double lx, double ly) const {
    for (const auto& e : entries_)
        if (contains_point(box_of(e->output, e->x, e->y), lx, ly))
            return e->output;
    return nullptr;
}

void OutputLayout::closest_point(const backend::Output* reference, double lx, double ly, double* cx,
                                 double* cy) const {
    double best_x = lx, best_y = ly, best = DBL_MAX;
    for (const auto& e : entries_) {
        if (reference && reference != e->output)
            continue;
        const Box b = box_of(e->output, e->x, e->y);
        if (b.width <= 0 || b.height <= 0)
            continue;
        // Inside the right and bottom edges by 1/256 px: still on the screen
        // after rounding to wl_fixed.
        const double x = std::clamp(lx, double(b.x), b.x + b.width - 1 / 256.0);
        const double y = std::clamp(ly, double(b.y), b.y + b.height - 1 / 256.0);
        const double d = (lx - x) * (lx - x) + (ly - y) * (ly - y);
        if (std::isfinite(d) && d < best) {
            best = d;
            best_x = x;
            best_y = y;
        }
    }
    *cx = best_x;
    *cy = best_y;
}

void OutputLayout::reconfigure() {
    // The automatic ones go right of the rightmost placed one, level with it.
    int max_x = INT_MIN, max_x_y = INT_MIN;
    for (const auto& e : entries_) {
        if (e->automatic)
            continue;
        const Box b = box_of(e->output, e->x, e->y);
        if (b.x + b.width > max_x) {
            max_x = b.x + b.width;
            max_x_y = b.y;
        }
    }
    if (max_x == INT_MIN)
        max_x = max_x_y = 0;
    for (const auto& e : entries_) {
        if (!e->automatic)
            continue;
        e->x = max_x;
        e->y = max_x_y;
        max_x += box_of(e->output, 0, 0).width;
    }
    change.emit();
}

} // namespace atrium
