#pragma once

#include "wlr.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace atrium {

class Server;

// org_kde_kwin_appmenu: where a Wayland window's menus are on D-Bus (a
// com.canonical.dbusmenu object), said by Qt and KDE apps, for the menu
// bar to show them. X11 apps say it on their window instead
// (_KDE_NET_WM_APPMENU_*), which the shell reads.
class AppMenus {
public:
    struct Address {
        std::string service, path;
    };

    explicit AppMenus(Server& server);
    ~AppMenus();
    AppMenus(const AppMenus&) = delete;
    AppMenus& operator=(const AppMenus&) = delete;

    // Where `surface`'s menus are, if its app said.
    const Address* for_surface(wlr_surface* surface) const;

private:
    struct Menu;
    static void bind(wl_client* client, void* data, uint32_t version, uint32_t id);
    void drop(Menu* menu);

    Server& server_;
    wl_global* global_ = nullptr;
    std::vector<wl_resource*> managers_;
    std::unordered_map<wlr_surface*, Menu*> menus_;
};

} // namespace atrium
