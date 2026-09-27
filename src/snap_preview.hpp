#pragma once
#include "scene/scene.hpp"
#include "wlr.hpp"

namespace atrium {

class Server;
class Space;

// The frosted rounded box that shows where a dragged window will land when
// released over a snap zone. It glides between zones and fades in and out.
class SnapPreview {
public:
    explicit SnapPreview(Server& server);
    ~SnapPreview();
    SnapPreview(const SnapPreview&) = delete;
    SnapPreview& operator=(const SnapPreview&) = delete;

    // Show at `target`, placed just under `below` (the dragged window's tree).
    void show(const wlr_box& target, scene::Node* below, const wlr_box& from);
    void hide();
    bool visible() const { return visible_; }
    // The space holding the preview is going away: take it back.
    void rescue(Space* space);

private:
    void set_box(const wlr_box& box, float alpha);

    Server& server_;
    scene::Tree* tree_ = nullptr;
    scene::Blur* blur_ = nullptr;
    scene::Rect* fill_ = nullptr;
    scene::Rect* ring_ = nullptr;
    wlr_box box_{};
    float alpha_ = 0;
    bool visible_ = false;
};

} // namespace atrium
