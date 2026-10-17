#pragma once
// Whether a panel that can be out of sight (the menu bar, the Dock) is out
// now. One rule for both, from Plasma's panels (PanelView::restoreAutoHide,
// showTemporarily) and what macOS 26 does (timed in a VM):
//
// - A panel that doesn't hide is simply out.
// - A hidden one comes out at once when the pointer has reached its edge (the
//   compositor says when: src/panel_edge_core.hpp), is over it, or it is
//   holding something open (a menu, a panel it opened, an icon being
//   dragged), and stays while any of those is so.
// - With none of them left it goes after 50 ms (Plasma's unhide timer; macOS
//   20-60 ms), so crossing from the panel to its menu doesn't drop it.
// - Summoned by a key it stays 3 s (Plasma), and longer while used. Summoned
//   again while out it goes at once, and stays away though the pointer is
//   still on it, until the pointer has left.
// - Peeked (an app in it wants attention) it comes out for the same 3 s, and
//   if already out it just stays at least that long.
// - Told to hide while nothing keeps it (a window goes fullscreen) it goes at
//   once: the 50 ms is for the pointer leaving, not for a change of mode.
#include <optional>

namespace atrium::reveal {

constexpr double kLeaveMs = 50;
constexpr double kSummonMs = 3000;

struct Inputs {
    bool hides = false;    // out of sight unless brought out
    bool edge = false;     // the pointer reached its edge and is still in its band
    bool hovered = false;  // the pointer is over the panel itself
    bool held = false;     // it has a menu or a panel open, or a drag going
};

class Reveal {
public:
    // An input changed, or the time asked for by wake_in() came. Returns
    // whether the panel is out.
    bool update(const Inputs& in, double now_ms);
    // The key: out for a while, or away if it is out. Returns the same.
    bool summon(double now_ms);
    // Out for a while to show something (an app asking for attention); unlike
    // the key it never sends the panel away.
    bool peek(double now_ms);
    bool out() const { return out_; }
    std::optional<double> wake_in(double now_ms) const;

private:
    bool wanted(double now_ms) const;

    Inputs in_;
    bool out_ = true;
    bool dismissed_ = false;  // sent away by the key: the pointer still on it doesn't count
    std::optional<double> summon_until_, leave_at_;
};

} // namespace atrium::reveal
