#include "wobbly_core.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

WobblyParams wobbly_preset(int level) {
    // stiffness, drag, move factor; velocity and acceleration bounds.
    static constexpr WobblyParams kSets[5] = {
        {0.15, 0.80, 0.10, 0.0, 1000.0, 0.5, 0.0, 1000.0, 0.5},
        {0.10, 0.85, 0.10, 0.0, 1000.0, 0.5, 0.0, 1000.0, 0.5},
        {0.06, 0.90, 0.10, 0.0, 1000.0, 0.5, 0.0, 1000.0, 0.5},
        {0.03, 0.92, 0.20, 0.0, 1000.0, 0.5, 0.0, 1000.0, 0.5},
        {0.01, 0.97, 0.25, 0.0, 1000.0, 0.5, 0.0, 1000.0, 0.5},
    };
    return kSets[std::clamp(level, 0, 4)];
}

namespace {

// The grid's resting places: a 4x4 lattice over the window.
void lattice(const FBox& r, std::array<FPoint, 16>& out) {
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            out[j * 4 + i] = {i == 3 ? r.x + r.width : r.x + r.width / 3 * i,
                              j == 3 ? r.y + r.height : r.y + r.height / 3 * j};
}

void bound(FPoint& v, double min, double max) {
    auto one = [&](double& c) {
        if (std::fabs(c) < min)
            c = 0;
        else if (std::fabs(c) > max)
            c = c > 0 ? max : -max;
    };
    one(v.x);
    one(v.y);
}

} // namespace

Wobbly::Wobbly(const FBox& rect, const WobblyParams& params, FPoint pointer, bool resize) : p_(params) {
    lattice(rect, origin_);
    position_ = origin_;
    const double xi = rect.width / 3, yi = rect.height / 3;
    const int ix = int((pointer.x - rect.x) / xi + 0.5), iy = int((pointer.y - rect.y) / yi + 0.5);
    held_[std::clamp(iy * kSide + ix, 0, kCount - 1)] = true;
    if (resize) {
        top_ = left_ = right_ = bottom_ = false;
        resize_from_ = rect;
    }
}

void Wobbly::sides_moved(const FBox& r) {
    const FBox& o = resize_from_;
    if (r.y != o.y)
        top_ = true;
    if (r.x != o.x)
        left_ = true;
    if (r.x + r.width != o.x + o.width)
        right_ = true;
    if (r.y + r.height != o.y + o.height)
        bottom_ = true;
}

void Wobbly::moved(const FBox& rect) {
    sides_moved(rect);
}

void Wobbly::release(const FBox& rect) {
    moving_ = false;
    sides_moved(rect);
}

bool Wobbly::advance(const FBox& rect, double ms) {
    while (ms > 0) {
        const double dt = std::min(ms, 10.0);
        ms -= dt;
        if (!step(rect, dt))
            return false;
    }
    return true;
}

// heightRingLinearMean: each value averaged with its (up to eight)
// neighbours, itself weighing as much as they do together.
void Wobbly::smooth(Grid& data) {
    Grid out{};
    for (int j = 0; j < kSide; j++)
        for (int i = 0; i < kSide; i++) {
            FPoint sum{};
            int n = 0;
            for (int dj = -1; dj <= 1; dj++)
                for (int di = -1; di <= 1; di++) {
                    const int x = i + di, y = j + dj;
                    if ((di || dj) && x >= 0 && x < kSide && y >= 0 && y < kSide) {
                        sum.x += data[y * kSide + x].x;
                        sum.y += data[y * kSide + x].y;
                        n++;
                    }
                }
            const FPoint v = data[j * kSide + i];
            out[j * kSide + i] = {(sum.x + n * v.x) / (2.0 * n), (sum.y + n * v.y) / (2.0 * n)};
        }
    data = out;
}

bool Wobbly::step(const FBox& rect, double time) {
    lattice(rect, origin_);
    const double xl = rect.width / 3, yl = rect.height / 3;
    const double k = p_.stiffness;

    // Each point is pulled towards its rest distance from each neighbour
    // along the line between them, and into line with it across; a held
    // point straight towards where the window has it.
    for (int j = 0; j < kSide; j++)
        for (int i = 0; i < kSide; i++) {
            const int idx = j * kSide + i;
            const FPoint pos = position_[idx];
            if (held_[idx]) {
                acceleration_[idx] = {(origin_[idx].x - pos.x) * k, (origin_[idx].y - pos.y) * k};
                continue;
            }
            FPoint a{};
            int n = 0;
            if (i > 0) {
                const FPoint nb = position_[idx - 1];
                a.x += (xl - (pos.x - nb.x)) * k;
                a.y += (nb.y - pos.y) * k;
                n++;
            }
            if (i < kSide - 1) {
                const FPoint nb = position_[idx + 1];
                a.x += ((nb.x - pos.x) - xl) * k;
                a.y += (nb.y - pos.y) * k;
                n++;
            }
            if (j > 0) {
                const FPoint nb = position_[idx - kSide];
                a.y += (yl - (pos.y - nb.y)) * k;
                a.x += (nb.x - pos.x) * k;
                n++;
            }
            if (j < kSide - 1) {
                const FPoint nb = position_[idx + kSide];
                a.y += ((nb.y - pos.y) - yl) * k;
                a.x += (nb.x - pos.x) * k;
                n++;
            }
            acceleration_[idx] = {a.x / n, a.y / n};
        }

    smooth(acceleration_);

    double acc_sum = 0, vel_sum = 0;
    for (int i = 0; i < kCount; i++) {
        FPoint acc = acceleration_[i];
        bound(acc, p_.min_acceleration, p_.max_acceleration);
        FPoint& vel = velocity_[i];
        vel.x = acc.x * time + vel.x * p_.drag;
        vel.y = acc.y * time + vel.y * p_.drag;
        acc_sum += std::fabs(acc.x) + std::fabs(acc.y);
    }

    smooth(velocity_);

    for (int i = 0; i < kCount; i++) {
        FPoint& vel = velocity_[i];
        bound(vel, p_.min_velocity, p_.max_velocity);
        position_[i].x += vel.x * time * p_.move_factor;
        position_[i].y += vel.y * time * p_.move_factor;
        vel_sum += std::fabs(vel.x) + std::fabs(vel.y);
    }

    // Sides that haven't moved in a resize stay put (KWin pins the three
    // rows, or columns, nearest that side).
    for (int a = 0; a < kSide; a++)
        for (int b = 0; b < kSide - 1; b++) {
            if (!top_)
                position_[a + kSide * b].y = origin_[a + kSide * b].y;
            if (!bottom_)
                position_[kSide * (kSide - 1) + a - kSide * b].y = origin_[kSide * (kSide - 1) + a - kSide * b].y;
            if (!left_)
                position_[a * kSide + b].x = origin_[a * kSide + b].x;
            if (!right_)
                position_[a * kSide + kSide - 1 - b].x = origin_[a * kSide + kSide - 1 - b].x;
        }

    wobbling_ = !(acc_sum < p_.stop_acceleration && vel_sum < p_.stop_velocity);
    return moving_ || wobbling_;
}

FPoint Wobbly::at(double u, double v) const {
    const double px[4] = {(1 - u) * (1 - u) * (1 - u), 3 * (1 - u) * (1 - u) * u, 3 * (1 - u) * u * u, u * u * u};
    const double py[4] = {(1 - v) * (1 - v) * (1 - v), 3 * (1 - v) * (1 - v) * v, 3 * (1 - v) * v * v, v * v * v};
    FPoint r{};
    for (int j = 0; j < kSide; j++)
        for (int i = 0; i < kSide; i++) {
            r.x += px[i] * py[j] * position_[i + j * kSide].x;
            r.y += px[i] * py[j] * position_[i + j * kSide].y;
        }
    return r;
}

} // namespace atrium
