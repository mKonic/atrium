#pragma once
#include "wlr.hpp"

#include <string>
#include <vector>

namespace atrium {

class Output;
class Server;
class View;

// A virtual desktop. Numbered spaces belong to one output and one of them is
// shown there at a time. Secret spaces are named, not tied to an output, and
// slide over whatever space is showing, on a dimmed backdrop, until toggled away.
class Space {
public:
    Space(Server& server, Output* output, int number);           // numbered
    Space(Server& server, std::string name);                     // secret
    ~Space();
    Space(const Space&) = delete;
    Space& operator=(const Space&) = delete;

    bool shown() const { return shown_; }
    // `linger` keeps a hidden space drawn so it can animate away; hide_now()
    // finishes the job.
    void set_shown(bool shown, bool linger = false);
    void hide_now();
    void set_offset(int dx, int dy);  // slide, for switching animations

    // Windows living here, in focus order.
    bool empty() const;

    // Show over `output` (secret spaces only): places the backdrop.
    void attach(Output* output);

    std::string id() const;     // "DP-1:3", "secret:communication"
    std::string label() const;  // "3", "communication"

    Server& server;
    Output* output;            // numbered: owner; secret: where it shows (or last showed)
    const int number;          // 0 for secret spaces
    const std::string name;    // empty for numbered spaces
    const bool secret;

    wlr_scene_tree* tree = nullptr;             // windows
    wlr_scene_tree* fullscreen_tree = nullptr;  // fullscreen windows, above panels
    wlr_scene_rect* backdrop = nullptr;         // secret spaces: the dimmed screen behind them
    wlr_scene_blur* backdrop_blur = nullptr;    // ... and everything under it, frosted

    wlr_ext_workspace_handle_v1* handle = nullptr;

    // Tiling (Mod+\): windows share the screen instead of floating, in this
    // order (window ids, oldest first); see Server::retile().
    bool tiled = false;
    std::vector<uint64_t> tile_order;
    // The dimmed, frosted desktop behind a tiled space's windows, made on
    // first use. Unlike a secret space's backdrop it takes no clicks.
    wlr_scene_rect* tile_dim = nullptr;
    wlr_scene_blur* tile_blur = nullptr;
    void ensure_tile_backdrop();

private:
    bool shown_ = false;
};

} // namespace atrium
