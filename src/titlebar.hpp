#pragma once
#include "wlr.hpp"

#include <cairo.h>

#include <string>
#include <vector>

namespace atrium {

class View;

// A title bar drawn by atrium for windows that let the compositor decorate
// them: round buttons at the top right, the title centered. A window with
// tabs has the tab bar under it, as on a Mac (or alone, over a window that
// draws its own title bar): a tab for each window, the one shown lit.
//
// Rendered with cairo/pango into a scene buffer at the output's scale, and
// re-rendered only when something it shows changes.
class Titlebar {
public:
    enum class Part { None, Close, Minimize, Maximize, Bar, Tab, TabClose };
    static constexpr int kHeight = 30;     // logical pixels
    static constexpr int kTabHeight = 28;  // the tab bar under it

    Titlebar(View& view, wlr_scene_tree* parent);
    ~Titlebar();
    Titlebar(const Titlebar&) = delete;
    Titlebar& operator=(const Titlebar&) = delete;

    // Redraw if width, title, focus, hover, press or scale changed.
    void update();

    // What is at (x, y) in title-bar coordinates; on the tab bar, `tab` says
    // which tab.
    Part part_at(double x, double y, int* tab = nullptr) const;

    void set_hover(Part part, int tab = -1);
    void set_pressed(Part part, int tab = -1);
    Part hover() const { return hover_; }
    Part pressed() const { return pressed_; }
    // A tab dragged over the bar: the line where it would go in, or -1.
    void set_drop(int slot);

    // The title and buttons: off for a window drawing its own, which has
    // only the tab bar from here.
    bool chrome() const { return chrome_; }
    void set_chrome(bool on);
    // Where the tab bar starts.
    int title_height() const;
    wlr_scene_buffer* node() const { return buffer_; }
    View& view() const { return view_; }

private:
    void render(int width, int height, float scale);
    void draw_title(cairo_t* cr, int width, int height);
    void draw_tabs(cairo_t* cr, int width, int top);

    View& view_;
    wlr_scene_buffer* buffer_ = nullptr;
    wlr_buffer* held_ = nullptr;  // keeps buffer_->buffer valid (see set_cairo_buffer)
    Part hover_ = Part::None;
    Part pressed_ = Part::None;
    int hover_tab_ = -1, pressed_tab_ = -1, drop_ = -1;
    bool chrome_ = true;

    // What the current buffer shows.
    struct Drawn {
        int width = -1, height = -1;
        float scale = 0;
        std::string title;
        bool active = false;
        Part hover = Part::None, pressed = Part::None;
        int title_height = 0;
        std::vector<std::string> tabs;  // each tab's title
        int current = -1, hover_tab = -1, pressed_tab = -1, drop = -1;
        uint64_t style = 0;  // hash of the appearance settings used
        bool operator==(const Drawn&) const = default;
    } drawn_;
};

} // namespace atrium
