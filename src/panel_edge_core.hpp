#pragma once
// When the pointer has reached the top or bottom edge of a screen, for the
// panel hidden there (the menu bar, the Dock). One rule for both, from what
// macOS 26 does (timed in a VM) and KWin's screen edges (screenedge.cpp):
//
// - The edge is the screen's outermost row of pixels, all the way across
//   (macOS; KWin takes only the panel's own width). A pixel in doesn't count.
// - The pointer has to rest there: 200 ms over the desktop (macOS: 190-200),
//   400 ms over a window the user made fullscreen. Passing over it does
//   nothing.
// - Never over fullscreen an app asked for itself (a video, a game), while an
//   app holds the pointer, or with a button down (macOS; KWin's
//   Edge::activatesForPointer): dragging a scroll bar or a selection to the
//   edge isn't asking for the panel. A drag and drop is: it has somewhere to
//   drop (KWin).
// - Once reached it stays reached while the pointer is anywhere in the
//   panel's band: the strip as deep as the panel, the whole width of the
//   screen (macOS keeps the Dock out beside it too). A button pressed there
//   changes nothing: that is a click on the panel.
#include <optional>

namespace atrium::panel_edge {

enum class Edge { None, Top, Bottom };
enum class Over { Desktop, UserFullscreen, AppFullscreen };

constexpr double kDwellMs = 200;
constexpr double kFullscreenDwellMs = 400;

struct Pointer {
    int screen = -1;   // which screen it is on; -1 for none
    int top = 0;       // that screen's first row
    int bottom = 0;    // one past its last
    double y = 0;
    Over over = Over::Desktop;
    bool blocked = false;  // held by an app, or a button is down outside a drag and drop
    int top_band = 1;      // how deep each panel is
    int bottom_band = 1;
};

class Tracker {
public:
    // The pointer moved, or the time asked for by wake_in() came.
    Edge update(const Pointer& p, double now_ms);
    Edge reached() const { return reached_; }
    int screen() const { return screen_; }
    // How long until update() should be called again though nothing moved.
    std::optional<double> wake_in(double now_ms) const;

private:
    Edge reached_ = Edge::None;
    Edge pending_ = Edge::None;
    double since_ = 0, dwell_ = 0;
    int screen_ = -1;
};

} // namespace atrium::panel_edge
