#pragma once
#include "wlr.hpp"

#include <string>

namespace atrium {

class View;

// A title bar drawn by atrium for windows that let the compositor decorate
// them: round buttons at the top right, the title centered.
//
// Rendered with cairo/pango into a scene buffer at the output's scale, and
// re-rendered only when something it shows changes.
class Titlebar {
public:
    enum class Part { None, Close, Minimize, Maximize, Bar };
    static constexpr int kHeight = 30;  // logical pixels

    Titlebar(View& view, wlr_scene_tree* parent);
    ~Titlebar();
    Titlebar(const Titlebar&) = delete;
    Titlebar& operator=(const Titlebar&) = delete;

    // Redraw if width, title, focus, hover, press or scale changed.
    void update();

    // What is at (x, y) in title-bar coordinates.
    Part part_at(double x, double y) const;

    void set_hover(Part part);
    void set_pressed(Part part);
    Part hover() const { return hover_; }
    Part pressed() const { return pressed_; }

    int height() const { return kHeight; }
    wlr_scene_buffer* node() const { return buffer_; }
    View& view() const { return view_; }

private:
    void render(int width, int height, float scale);

    View& view_;
    wlr_scene_buffer* buffer_ = nullptr;
    wlr_buffer* held_ = nullptr;  // keeps buffer_->buffer valid (see set_cairo_buffer)
    Part hover_ = Part::None;
    Part pressed_ = Part::None;

    // What the current buffer shows.
    struct Drawn {
        int width = -1, height = -1;
        float scale = 0;
        std::string title;
        bool active = false;
        Part hover = Part::None, pressed = Part::None;
        uint64_t style = 0;  // hash of the appearance settings used
        bool operator==(const Drawn&) const = default;
    } drawn_;
};

} // namespace atrium
