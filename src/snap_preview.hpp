#pragma once
#include "scene/scene.hpp"
#include "common.hpp"

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
    void show(const Box& target, scene::Node* below, const Box& from);
    void hide();
    bool visible() const { return visible_; }
    // The space holding the preview is going away: take it back.
    void rescue(Space* space);

private:
    void set_box(const Box& box, float alpha);

    Server& server_;
    scene::Tree* tree_ = nullptr;
    scene::Blur* blur_ = nullptr;
    scene::Rect* fill_ = nullptr;
    scene::Rect* ring_ = nullptr;
    Box box_{};
    float alpha_ = 0;
    bool visible_ = false;
};

} // namespace atrium
