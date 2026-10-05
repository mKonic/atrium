#include "appmenu.hpp"

#include "server.hpp"
#include "view.hpp"

#include "appmenu-protocol.h"

#include <algorithm>

namespace atrium {

struct AppMenus::Menu {
    AppMenus* owner;
    wlr_surface* surface;
    wl_resource* resource;
    Address address;
    wl_listener surface_gone{};
};

namespace {

void destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

} // namespace

AppMenus::AppMenus(Server& server) : server_(server) {
    global_ = wl_global_create(server_.display, &org_kde_kwin_appmenu_manager_interface, 2, this, bind);
}

AppMenus::~AppMenus() {
    for (wl_resource* m : managers_)
        wl_resource_set_user_data(m, nullptr);
    while (!menus_.empty())
        drop(menus_.begin()->second);
    if (global_)
        wl_global_destroy(global_);
}

const AppMenus::Address* AppMenus::for_surface(wlr_surface* surface) const {
    const auto it = menus_.find(surface);
    return it == menus_.end() || it->second->address.service.empty() ? nullptr : &it->second->address;
}

void AppMenus::drop(Menu* menu) {
    wl_list_remove(&menu->surface_gone.link);
    if (menu->resource)
        wl_resource_set_user_data(menu->resource, nullptr);
    if (auto it = menus_.find(menu->surface); it != menus_.end() && it->second == menu)
        menus_.erase(it);
    delete menu;
}

void AppMenus::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<AppMenus*>(data);
    wl_resource* manager = wl_resource_create(client, &org_kde_kwin_appmenu_manager_interface, int(version), id);
    if (!manager) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct org_kde_kwin_appmenu_manager_interface impl = {
        .create =
            [](wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surface_resource) {
                auto* self = static_cast<AppMenus*>(wl_resource_get_user_data(manager));
                wl_resource* r = wl_resource_create(client, &org_kde_kwin_appmenu_interface,
                                                    wl_resource_get_version(manager), id);
                if (!r) {
                    wl_client_post_no_memory(client);
                    return;
                }
                static const struct org_kde_kwin_appmenu_interface menu_impl = {
                    .set_address =
                        [](wl_client*, wl_resource* resource, const char* service, const char* path) {
                            auto* menu = static_cast<Menu*>(wl_resource_get_user_data(resource));
                            if (!menu)
                                return;
                            menu->address = {service ? service : "", path ? path : ""};
                            // The menu bar hears it with the window.
                            if (View* view = Server::owner_of(menu->surface).view)
                                menu->owner->server_.notify_window(*view, "changed");
                        },
                    .release = destroy_resource,
                };
                if (!self) {  // atrium is going
                    wl_resource_set_implementation(r, &menu_impl, nullptr, nullptr);
                    return;
                }
                wlr_surface* surface = wlr_surface_from_resource(surface_resource);
                // One menu per surface: a new one replaces the old.
                if (auto it = self->menus_.find(surface); it != self->menus_.end())
                    self->drop(it->second);
                auto* menu = new Menu{self, surface, r, {}};
                menu->surface_gone.notify = [](wl_listener* l, void*) {
                    Menu* m = wl_container_of(l, m, surface_gone);
                    m->owner->drop(m);
                };
                wl_signal_add(&surface->events.destroy, &menu->surface_gone);
                self->menus_[surface] = menu;
                wl_resource_set_implementation(r, &menu_impl, menu, [](wl_resource* resource) {
                    if (auto* m = static_cast<Menu*>(wl_resource_get_user_data(resource))) {
                        m->resource = nullptr;
                        m->owner->drop(m);
                    }
                });
            },
        .release = destroy_resource,
    };
    wl_resource_set_implementation(manager, &impl, self, [](wl_resource* resource) {
        if (auto* s = static_cast<AppMenus*>(wl_resource_get_user_data(resource)))
            std::erase(s->managers_, resource);
    });
    self->managers_.push_back(manager);
}

} // namespace atrium
