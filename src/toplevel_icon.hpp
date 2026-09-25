#pragma once

#include <wayland-server-core.h>

#include <vector>

namespace atrium {

class Server;

// xdg-toplevel-icon-v1, atrium's own: wlroots' sends wl_buffer.release when
// an app adds a second picture of the same size, which the protocol says is
// never sent. GTK destroys a buffer on release and again with the icon, and
// crashes; with every window of the app, when it is one process (Ghostty).
// Here a picture stays held until the app destroys its buffer, so release
// never goes out.
class ToplevelIcons {
public:
    explicit ToplevelIcons(Server& server);
    ~ToplevelIcons();
    ToplevelIcons(const ToplevelIcons&) = delete;
    ToplevelIcons& operator=(const ToplevelIcons&) = delete;

private:
    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    static void manager_gone(wl_resource* resource);
    static void create_icon(wl_client* client, wl_resource* manager, uint32_t id);
    static void set_icon(wl_client* client, wl_resource* manager, wl_resource* toplevel, wl_resource* icon);

    Server& server_;
    wl_global* global_ = nullptr;
    std::vector<wl_resource*> managers_;
};

} // namespace atrium
