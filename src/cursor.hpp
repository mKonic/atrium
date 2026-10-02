#pragma once
// The pointer's position in the output layout and its image on every screen:
// in the screen's cursor plane where it has one (rendered to the plane's
// size and the screen's scale and rotation), else drawn by the scene into
// each frame. After wlroots' wlr_cursor and wlr_output_cursor (MIT).
#include "output_layout.hpp"

#include <memory>
#include <string>
#include <vector>

struct wlr_render_pass;
struct wlr_texture;
struct wlr_xcursor_manager;

namespace atrium {

class Cursor {
public:
    Cursor(OutputLayout& layout, wl_event_loop* loop);
    ~Cursor();
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    double x = 0, y = 0;

    // To the screen point nearest (lx, ly).
    void warp_closest(double lx, double ly);
    // To (lx, ly) if it is on a screen; false (and unmoved) if not.
    bool warp(double lx, double ly);
    // By (dx, dy), kept on the screens.
    void move(double dx, double dy);
    // A point of the whole layout from a fraction of it (0..1 each way).
    void absolute_to_layout(double fx, double fy, double* lx, double* ly) const;

    // The image: a theme's cursor (animated if it is), a client's buffer
    // (hotspot in buffer pixels, at `scale` buffer pixels per logical one),
    // or none.
    void set_xcursor(wlr_xcursor_manager* manager, const char* name);
    void set_buffer(wlr_buffer* buffer, int hotspot_x, int hotspot_y, float scale);
    void unset_image();

    // Draws it into a frame of `output` where it has no cursor plane
    // (`damage` in the frame's buffer pixels).
    void render(const backend::Output* output, wlr_render_pass* pass, const pixman_region32_t* damage);
    // The renderer or allocator changed (GPU reset): everything made again.
    void reset_render();
    // Whether `output` shows it in its cursor plane.
    bool in_plane(const backend::Output* output) const;

private:
    struct Screen;
    enum class Kind { None, XCursor, Buffer };

    Screen* screen_of(const backend::Output* o) const;
    void sync_screens();
    void refresh(Screen& s);       // a new image, or the screen changed
    void place(Screen& s);         // the image follows the position
    bool try_plane(Screen& s);
    void damage(Screen& s);        // where it was drawn in software
    wlr_box box_on(const Screen& s) const;  // in the screen's transformed pixels
    void refresh_all();
    void schedule_animation();

    OutputLayout& layout_;
    wl_event_loop* loop_;
    wl::Connection layout_change_;
    std::vector<std::unique_ptr<Screen>> screens_;

    Kind kind_ = Kind::None;
    wlr_xcursor_manager* manager_ = nullptr;
    std::string name_;
    wlr_buffer* buffer_ = nullptr;  // locked
    int hot_x_ = 0, hot_y_ = 0;
    float buffer_scale_ = 1;
    size_t frame_ = 0;  // an animated theme cursor's image
    wl_event_source* animation_ = nullptr;
};

} // namespace atrium
