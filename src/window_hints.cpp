// What windows say about themselves beyond xdg-shell: an icon, a tag, that
// a dialog is modal, what kind of content they show and whether they would
// rather tear than wait for vblank. Plus the sandbox line for apps that
// arrive through security-context.
#include "output.hpp"
#include "sandbox.hpp"
#include "server.hpp"
#include "toplevel_icon.hpp"
#include "view.hpp"
#ifdef ATRIUM_XWAYLAND
#include "xwayland/server.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace atrium {

void Server::setup_window_hints() {
    toplevel_icons = std::make_unique<ToplevelIcons>(*this);

    connections_.push_back(wl->tags->set_tag.connect([this](const wl::ToplevelTags::Event& e) {
        if (View* v = e.toplevel ? static_cast<View*>(e.toplevel->data) : nullptr) {
            v->tag = e.tag;
            if (v->mapped)
                notify_window(*v, "changed");
        }
    }));

    // Sandboxed apps don't get to see the screen, other windows or the
    // clipboard behind their back, fake input, or change the desktop. And
    // xwayland_shell_v1 is Xwayland's alone.
    wl_display_set_global_filter(display, [](const wl_client* client, const wl_global* global, void* data) {
        auto* server = static_cast<Server*>(data);
        const char* name = wl_global_get_interface(global)->name;
#ifdef ATRIUM_XWAYLAND
        if (std::string_view(name) == "xwayland_shell_v1")
            return server->xwayland && client == server->xwayland->client();
#endif
        if (!server->wl->security->lookup(client))
            return true;
        return !privileged_protocol(name);
    }, this);
}

void forget_icon(const View& view) {
    if (view.icon.starts_with('/')) {
        std::error_code ec;
        std::filesystem::remove(view.icon, ec);
    }
}

const char* content_type_name(Server& server, const View& view) {
    (void)server;
    if (!view.surface())
        return "none";
    switch (view.surface()->current().content_type) {  // wp_content_type_v1.type
    case 1: return "photo";
    case 2: return "video";
    case 3: return "game";
    default: return "none";
    }
}

View* tearing_view(Server& server, const Output& output) {
    if (!server.config.allow_tearing)
        return nullptr;
    // The front window, if it is fullscreen here and asked for it.
    for (View* v : server.views) {
        if (v->output != &output || !v->visible())
            continue;
        if (!v->fullscreen || !v->surface())
            return nullptr;
        return v->surface()->current().presentation_hint == 1 ? v : nullptr;  // async
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
                          v->surface()->current().presentation_hint == 1;
        return game ? v : nullptr;
    }
    return nullptr;
}

} // namespace atrium
