#pragma once
#include <pixman.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <utility>

namespace atrium::wl {

// A pixman region that owns itself: copyable, movable, freed with it.
class Region {
public:
    Region() { pixman_region32_init(&r_); }
    Region(int x, int y, int w, int h) { pixman_region32_init_rect(&r_, x, y, unsigned(w), unsigned(h)); }
    explicit Region(const pixman_region32_t* other) {
        pixman_region32_init(&r_);
        pixman_region32_copy(&r_, other);
    }
    Region(const Region& other) : Region(&other.r_) {}
    Region& operator=(const Region& other) {
        if (this != &other)
            pixman_region32_copy(&r_, &other.r_);
        return *this;
    }
    Region(Region&& other) noexcept : Region() { swap(other); }
    Region& operator=(Region&& other) noexcept {
        swap(other);
        return *this;
    }
    ~Region() { pixman_region32_fini(&r_); }

    // Everything (an input region nobody set).
    static Region infinite() { return Region(INT32_MIN / 2, INT32_MIN / 2, INT32_MAX, INT32_MAX); }

    void swap(Region& other) noexcept { std::swap(r_, other.r_); }
    void clear() { pixman_region32_clear(&r_); }
    bool empty() const { return !pixman_region32_not_empty(&r_); }

    Region& add(int x, int y, int w, int h) {
        if (w > 0 && h > 0)
            pixman_region32_union_rect(&r_, &r_, x, y, unsigned(w), unsigned(h));
        return *this;
    }
    Region& subtract(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0)
            return *this;
        Region r(x, y, w, h);
        pixman_region32_subtract(&r_, &r_, &r.r_);
        return *this;
    }
    Region& add(const Region& other) {
        pixman_region32_union(&r_, &r_, &other.r_);
        return *this;
    }
    Region& intersect(int x, int y, int w, int h) {
        pixman_region32_intersect_rect(&r_, &r_, x, y, unsigned(std::max(w, 0)), unsigned(std::max(h, 0)));
        return *this;
    }
    Region& translate(int dx, int dy) {
        pixman_region32_translate(&r_, dx, dy);
        return *this;
    }
    bool contains(int x, int y) const { return pixman_region32_contains_point(&r_, x, y, nullptr); }
    pixman_box32_t extents() const { return *pixman_region32_extents(&r_); }

    pixman_region32_t* get() { return &r_; }
    const pixman_region32_t* get() const { return &r_; }

private:
    pixman_region32_t r_;
};

} // namespace atrium::wl
