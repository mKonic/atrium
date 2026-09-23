#pragma once
#include "wlr.hpp"

#include <string>

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
    void set_shown(bool shown);

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

    wlr_ext_workspace_handle_v1* handle = nullptr;

private:
    bool shown_ = false;
};

} // namespace atrium
