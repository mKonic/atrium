#include "reveal_core.hpp"

#include <algorithm>

namespace atrium::reveal {

bool Reveal::wanted(double now_ms) const {
    if (!in_.hides || in_.held)
        return true;
    if (summon_until_ && now_ms < *summon_until_)
        return true;
    return !dismissed_ && (in_.edge || in_.hovered);
}

bool Reveal::update(const Inputs& in, double now_ms) {
    const bool began_hiding = in.hides && !in_.hides;
    in_ = in;
    if (!in.hides)
        summon_until_.reset();
    if (!in.edge && !in.hovered)
        dismissed_ = false;
    if (summon_until_ && now_ms >= *summon_until_)
        summon_until_.reset();

    if (wanted(now_ms)) {
        out_ = true;
        leave_at_.reset();
    } else if (out_) {
        if (began_hiding) {
            out_ = false;
            leave_at_.reset();
        } else if (!leave_at_) {
            leave_at_ = now_ms + kLeaveMs;
        } else if (now_ms >= *leave_at_) {
            out_ = false;
            leave_at_.reset();
        }
    }
    return out_;
}

bool Reveal::summon(double now_ms) {
    if (!in_.hides)
        return out_;
    if (out_ && !in_.held) {
        summon_until_.reset();
        leave_at_.reset();
        dismissed_ = in_.edge || in_.hovered;
        out_ = false;
        return out_;
    }
    dismissed_ = false;
    summon_until_ = now_ms + kSummonMs;
    leave_at_.reset();
    out_ = true;
    return out_;
}

std::optional<double> Reveal::wake_in(double now_ms) const {
    std::optional<double> at = leave_at_;
    if (summon_until_ && (!at || *summon_until_ < *at))
        at = summon_until_;
    if (!at)
        return std::nullopt;
    return std::max(1.0, *at - now_ms);
}

} // namespace atrium::reveal
