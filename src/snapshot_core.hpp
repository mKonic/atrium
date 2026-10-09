#pragma once
// The geometry of window animations drawn from a snapshot of the window:
// its parts (buffers, title bar, shadow, outline) keep their places relative
// to the frame while the frame is drawn into another box, as KWin's Scale,
// Size and Translation do to a window's quads.

namespace atrium {

struct FBox {
    double x = 0, y = 0, width = 0, height = 0;
    bool operator==(const FBox&) const = default;
};

// Where a part at `part` (relative to a frame `frame_w` x `frame_h`, origin
// at the frame's top left) goes when the frame is drawn into `to`.
FBox snapshot_map(const FBox& part, double frame_w, double frame_h, const FBox& to);

// Hyprland's popin (caelestia's windowsIn/windowsOut use it with no
// percentage): `goal` shrunk to `fraction` of its size about its centre,
// never smaller than 5x5.
FBox popin_box(const FBox& goal, double fraction);

FBox lerp(const FBox& a, const FBox& b, double t);

struct FPoint {
    double x = 0, y = 0;
};

// Which side of the window its icon is on, as KWin's Magic Lamp decides it:
// for a Dock along the bottom or top, the side of the screen's middle the
// icon is on; for one along the left or right, likewise across.
enum class GenieEdge { Top, Bottom, Left, Right };
GenieEdge genie_edge(const FBox& screen, const FBox& dock, const FBox& icon);

// KWin's Magic Lamp: where the point (qx, qy) of a window (relative to its
// frame `geo`'s top left) is drawn at `progress` (0 shown, 1 in the icon),
// relative to the same top left. Rows nearer the icon move first (by the
// cube of how far along they are) and narrow to the icon's width as they
// near it. Its grid is 40 px.
FPoint genie_point(GenieEdge edge, const FBox& geo, const FBox& icon, double progress, double qx, double qy);
constexpr double kGenieCell = 40;

} // namespace atrium
