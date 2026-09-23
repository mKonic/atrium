#pragma once
#include "wlr.hpp"

#include <memory>
#include <vector>

namespace atrium {

class Output;
class Server;
class View;

// Mission Control: every window of the shown spaces scaled out side by side
// over a frosted desktop, live. Click one (or pick it with the arrow keys and
// Return) to go to it; Escape or a click on the desktop goes back.
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
    void open();
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

private:
    enum class State { Closed, Open, Closing };

    struct Piece {
        wlr_scene_buffer* node;
        int x, y, w, h;  // in the window's frame, unscaled
        fx_corner_radii corners;
    };
    struct Screen {
        Output* output;
        wlr_scene_tree* tree;
        wlr_scene_blur* blur;
        wlr_scene_rect* dim;
    };
    struct Thumb {
        View* view;
        Screen* screen;
        wlr_scene_tree* tree;        // at cur.x/cur.y
        wlr_scene_rect* ring;        // highlight around the hovered one
        wlr_scene_shadow* shadow;
        wlr_scene_blur* blur;        // frosted glass behind translucent windows
        wlr_scene_tree* pieces_tree;
        std::vector<Piece> pieces;
        wlr_scene_buffer* label;     // title pill, under the hovered one
        wlr_box from{}, to{}, cur{};
        int laid_w = 0, laid_h = 0;  // window size the layout was made for
        int label_w = 0;
    };

    Screen* screen_for(Output* output);
    Thumb* thumb_for(View* view);
    Thumb* thumb_at(double lx, double ly);
    void navigate(int dx, int dy);
    void add_thumb(View* view, Screen* screen);
    void remove_thumb(Thumb* thumb);
    void snapshot(Thumb& thumb);
    void place(Thumb& thumb, const wlr_box& box);
    void set_highlight(Thumb* thumb);
    void render_label(Thumb& thumb);
    // New targets for every thumb, animated from wherever they are now.
    void relayout();
    void set_fade(double a);
    void finish_close();
    void destroy_all();
    bool included(View* view) const;

    Server& server_;
    State state_ = State::Closed;
    wlr_scene_tree* root_ = nullptr;
    std::vector<std::unique_ptr<Screen>> screens_;
    std::vector<std::unique_ptr<Thumb>> thumbs_;
    Thumb* highlight_ = nullptr;
    double fade_ = 0;  // how far the desktop has frosted over, 0..1
};

} // namespace atrium
