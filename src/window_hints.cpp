// What windows say about themselves beyond xdg-shell: an icon, a tag, that
// a dialog is modal, what kind of content they show and whether they would
// rather tear than wait for vblank. Plus the sandbox line for apps that
// arrive through security-context.
#include "output.hpp"
#include "sandbox.hpp"
#include "server.hpp"
#include "toplevel_icon.hpp"
#include "view.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace atrium {

namespace {

View* view_of(wlr_xdg_toplevel* toplevel) {
    return toplevel && toplevel->base ? static_cast<View*>(toplevel->base->data) : nullptr;
}

} // namespace

void Server::setup_window_hints() {
    wlr_xdg_wm_dialog_v1_create(display, 1);  // read through View::modal()

    toplevel_icons = std::make_unique<ToplevelIcons>(*this);

    auto* tags = wlr_xdg_toplevel_tag_manager_v1_create(display, 1);
    set_tag_.connect(&tags->events.set_tag, [this](wlr_xdg_toplevel_tag_manager_v1_set_tag_event* e) {
        if (View* v = view_of(e->toplevel)) {
            v->tag = e->tag ? e->tag : "";
            if (v->mapped)
                notify_window(*v, "changed");
        }
    });

    content_type_manager = wlr_content_type_manager_v1_create(display, 1);
    tearing_manager = wlr_tearing_control_manager_v1_create(display, 1);

    // Sandboxed apps don't get to see the screen, other windows or the
    // clipboard behind their back, fake input, or change the desktop.
    security_context_manager = wlr_security_context_manager_v1_create(display);
    wl_display_set_global_filter(display, [](const wl_client* client, const wl_global* global, void* data) {
        auto* manager = static_cast<wlr_security_context_manager_v1*>(data);
        if (!wlr_security_context_manager_v1_lookup_client(manager, client))
            return true;
        return !privileged_protocol(wl_global_get_interface(global)->name);
    }, security_context_manager);
}

void forget_icon(const View& view) {
    if (view.icon.starts_with('/')) {
        std::error_code ec;
        std::filesystem::remove(view.icon, ec);
    }
}

const char* content_type_name(Server& server, const View& view) {
    if (!server.content_type_manager || !view.surface())
        return "none";
    switch (wlr_surface_get_content_type_v1(server.content_type_manager, view.surface())) {
    case WP_CONTENT_TYPE_V1_TYPE_PHOTO: return "photo";
    case WP_CONTENT_TYPE_V1_TYPE_VIDEO: return "video";
    case WP_CONTENT_TYPE_V1_TYPE_GAME: return "game";
    default: return "none";
    }
}

View* tearing_view(Server& server, const Output& output) {
    if (!server.config.allow_tearing || !server.tearing_manager)
        return nullptr;
    // The front window, if it is fullscreen here and asked for it.
    for (View* v : server.views) {
        if (v->output != &output || !v->visible())
            continue;
        if (!v->fullscreen || !v->surface())
            return nullptr;
        return wlr_tearing_control_manager_v1_surface_hint_from_surface(server.tearing_manager, v->surface()) ==
                       WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC
                   ? v
                   : nullptr;
    }
    return nullptr;
}

View* game_view(Server& server, const Output& output) {
    // The front window, if it is fullscreen here and says it's a game, or
    // asks to tear (only games do).
    for (View* v : server.views) {
        if (v->output != &output || !v->visible())
            continue;
        if (!v->fullscreen || !v->surface())
            return nullptr;
        const bool game = std::string_view(content_type_name(server, *v)) == "game" ||
                          (server.tearing_manager &&
                           wlr_tearing_control_manager_v1_surface_hint_from_surface(server.tearing_manager, v->surface()) ==
                               WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
        return game ? v : nullptr;
    }
    return nullptr;
}

} // namespace atrium
