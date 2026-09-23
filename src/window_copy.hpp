#pragma once
#include "wlr.hpp"

#include <vector>

namespace atrium {

class View;

// A miniature of a window: copies of its scene buffers (title bar, surfaces,
// popups) scaled into a box. The copies hold references to the client's
// buffers, so they stay valid after the client moves on; refresh() picks up
// what the window shows now. Must not outlive the view.
class WindowCopy {
public:
    WindowCopy(View& view, wlr_scene_tree* parent);
    ~WindowCopy();
    WindowCopy(const WindowCopy&) = delete;
    WindowCopy& operator=(const WindowCopy&) = delete;

    void refresh();
    // Scale into width x height at the tree's origin.
    void place(int width, int height);

    View& view() const { return view_; }
    wlr_scene_tree* tree() const { return tree_; }

private:
    struct Piece {
        wlr_scene_buffer* node;
        int x, y, w, h;  // in the window's frame, unscaled
        fx_corner_radii corners;
    };

    View& view_;
    wlr_scene_tree* tree_;
    wlr_scene_tree* inner_ = nullptr;  // rebuilt by refresh()
    std::vector<Piece> pieces_;
    int width_ = 0, height_ = 0;
};

} // namespace atrium
