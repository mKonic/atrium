#pragma once
// Frosted blur behind a popup (an app's menu, an input method's candidate
// window) while blur and transparency are on: Hyprland's blur:popups and
// blur:input_methods, blurring only where the popup's alpha is above 0.2
// (their ignorealpha), so a menu's soft shadow stays clear. Lives as long as
// the surface; its blur node goes with the popup's scene tree.
#include "listener.hpp"
#include "wlr.hpp"

namespace atrium {

class Server;

class PopupBlur {
public:
    // Deletes itself when `surface` goes.
    static void attach(Server& server, wlr_scene_tree* tree, wlr_surface* surface);

private:
    PopupBlur(Server& server, wlr_scene_tree* tree, wlr_surface* surface);
    void update();

    Server& server_;
    wlr_scene_tree* tree_;
    wlr_surface* surface_;
    wlr_scene_blur* blur_ = nullptr;
    Listener<> commit_;
    Listener<> destroy_;
};

} // namespace atrium
