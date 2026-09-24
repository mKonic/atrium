#pragma once
#include "config.hpp"
#include "wlr.hpp"

#include <memory>
#include <vector>

namespace atrium {

class Output;
class Server;
class View;
class WindowCopy;

// Mission Control: every window of the shown spaces scaled out side by side
// over a frosted desktop, live. Click one (or pick it with the arrow keys and
// Return) to go to it; Escape or a click on the desktop goes back. A strip of
// the output's spaces runs along the top: click one to look at it, drop a
// window on one to send it there.
//
// Thumbnails are copies of the windows' buffers, refreshed on every commit;
// the real windows stay where they are, invisible, so they keep drawing.
class Overview {
public:
    explicit Overview(Server& server);
    ~Overview();
    Overview(const Overview&) = delete;
    Overview& operator=(const Overview&) = delete;

    bool active() const { return state_ != State::Closed; }
    void toggle();
    // `animate` false shows the finished overview at once (switching spaces
    // from inside it).
    void open(bool animate = true);
    // Only `app_id`'s windows (app exposé). Again for the same app closes it.
    void open_app(const std::string& app_id);
    // Animate back; `pick` (if any) is focused and raised first.
    void close(View* pick = nullptr);
    // Drop everything at once (space switch, lock, teardown).
    void close_now();

    // Input while active. All of it belongs to the overview.
    void motion(double lx, double ly);
    void button(double lx, double ly, uint32_t button, bool pressed);
    void key(xkb_keysym_t sym);

    // Window changes while active.
    void view_changed(View* view);  // new content: re-copy its buffers
    void view_mapped(View* view);
    void view_unmapped(View* view);
    void output_removed(Output* output);
    void spaces_changed();  // spaces came, went or changed hands: redo the strip

private:
    enum class State { Closed, Open, Closing };

    struct Screen {
        Output* output;
        wlr_scene_tree* tree;
        wlr_scene_blur* blur;
        wlr_scene_rect* dim;
        wlr_scene_tree* strip = nullptr;  // space tiles, under the thumbnails
    };
    // One space in the strip.
    struct Tile {
        Screen* screen;
        int number;
        wlr_scene_tree* tree;
        wlr_scene_rect* ring;
        wlr_box box;
        bool current;
    };
    struct Thumb {
        View* view;
        Screen* screen;
        wlr_scene_tree* tree;        // at cur.x/cur.y
        wlr_scene_rect* ring;        // highlight around the hovered one
        wlr_scene_shadow* shadow;
        wlr_scene_blur* blur;        // frosted glass behind translucent windows
        wlr_scene_rect* backing;     // or a solid fill, with transparency off
        std::unique_ptr<WindowCopy> copy;
        wlr_scene_buffer* label;     // title pill, under the hovered one
        wlr_box from{}, to{}, cur{};
        int laid_w = 0, laid_h = 0;  // window size the layout was made for
        int label_w = 0;
    };

    Screen* screen_for(Output* output);
    Thumb* thumb_for(View* view);
    Thumb* thumb_at(double lx, double ly);
    void navigate(int dx, int dy);
    Tile* tile_at(double lx, double ly);
    void build_strip(Screen& screen);
    void rebuild_strips();
    void set_tile_hover(Tile* tile);
    void go_to_space(Output* output, int number);
    void drop(Thumb* thumb, double lx, double ly);
    void add_thumb(View* view, Screen* screen);
    void remove_thumb(Thumb* thumb);
    void snapshot(Thumb& thumb);
    void place(Thumb& thumb, const wlr_box& box);
    void set_highlight(Thumb* thumb);
    void render_label(Thumb& thumb);
    // New targets for every thumb, animated from wherever they are now.
    void relayout(bool animate = true);
    void set_fade(double a);
    void finish_close();
    void destroy_all();
    bool included(View* view) const;
    Color ring_color() const;

    Server& server_;
    State state_ = State::Closed;
    std::string app_;  // app exposé: only this app's windows
    wlr_scene_tree* root_ = nullptr;
    std::vector<std::unique_ptr<Screen>> screens_;
    std::vector<std::unique_ptr<Thumb>> thumbs_;
    Thumb* highlight_ = nullptr;
    std::vector<std::unique_ptr<Tile>> tiles_;
    Tile* tile_hover_ = nullptr;

    // A press on a thumbnail picks it on release, or drags it past a few pixels.
    Thumb* press_thumb_ = nullptr;
    Tile* press_tile_ = nullptr;
    bool pressed_ = false;
    double press_x_ = 0, press_y_ = 0;
    Thumb* drag_ = nullptr;
    double drag_rx_ = 0, drag_ry_ = 0;  // where in the dragged thumbnail the cursor holds it, 0..1
    double fade_ = 0;  // how far the desktop has frosted over, 0..1
};

} // namespace atrium
