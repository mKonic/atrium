#pragma once
#include "scene/scene.hpp"
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
    WindowCopy(View& view, scene::Tree* parent);
    ~WindowCopy();
    WindowCopy(const WindowCopy&) = delete;
    WindowCopy& operator=(const WindowCopy&) = delete;

    void refresh();
    // Scale into width x height at the tree's origin.
    void place(int width, int height);

    View& view() const { return view_; }
    scene::Tree* tree() const { return tree_; }

private:
    struct Piece {
        scene::Buffer* node;
        int x, y, w, h;  // in the window's frame, unscaled
        scene::Radii corners;
    };

    View& view_;
    scene::Tree* tree_;
    scene::Tree* inner_ = nullptr;  // rebuilt by refresh()
    std::vector<Piece> pieces_;
    int width_ = 0, height_ = 0;
};

} // namespace atrium
