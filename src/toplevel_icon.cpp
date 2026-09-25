#include "toplevel_icon.hpp"

#include "server.hpp"
#include "view.hpp"

#include "xdg-toplevel-icon-v1-protocol.h"

#include <cairo.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace atrium {

namespace {

namespace fs = std::filesystem;

struct Icon;

// A picture an app gave an icon, held (locked) from add_buffer until the app
// destroys the buffer: dropping the last lock earlier sends release.
struct Held {
    wlr_buffer* buffer = nullptr;
    int scale = 1;
    Icon* icon = nullptr;  // null once the icon is gone or replaced it
    wl_listener destroy{};
};

struct Icon {
    std::string name;
    std::vector<Held*> pictures;
    bool immutable = false;  // assigned to a toplevel
};

Icon* icon_from(wl_resource* resource) {
    return static_cast<Icon*>(wl_resource_get_user_data(resource));
}

void destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

void buffer_gone(wl_listener* listener, void*) {
    Held* h = wl_container_of(listener, h, destroy);
    wl_list_remove(&h->destroy.link);
    if (h->icon)
        std::erase(h->icon->pictures, h);
    wlr_buffer_unlock(h->buffer);  // the resource is gone: nothing is sent
    delete h;
}

void icon_set_name(wl_client*, wl_resource* resource, const char* name) {
    Icon* icon = icon_from(resource);
    if (icon->immutable) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                               "the icon was already assigned to a toplevel");
        return;
    }
    icon->name = name;
}

void icon_add_buffer(wl_client*, wl_resource* resource, wl_resource* buffer_resource, int32_t scale) {
    Icon* icon = icon_from(resource);
    if (icon->immutable) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                               "the icon was already assigned to a toplevel");
        return;
    }
    wlr_buffer* buffer = wlr_buffer_try_from_resource(buffer_resource);
    wlr_shm_attributes shm{};
    if (!buffer || !wlr_buffer_get_shm(buffer, &shm) || buffer->width != buffer->height) {
        if (buffer)
            wlr_buffer_unlock(buffer);
        wl_resource_post_error(resource, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
                               "icon buffers must be square and backed by wl_shm");
        return;
    }
    // The same size and scale again replaces the picture; the old buffer
    // stays held until the app destroys it.
    for (Held*& h : icon->pictures)
        if (h->buffer->width == buffer->width && h->scale == scale) {
            h->icon = nullptr;
            h = nullptr;
        }
    std::erase(icon->pictures, nullptr);
    auto* h = new Held{.buffer = buffer, .scale = scale, .icon = icon};
    h->destroy.notify = buffer_gone;
    wl_resource_add_destroy_listener(buffer_resource, &h->destroy);
    icon->pictures.push_back(h);
}

void icon_gone(wl_resource* resource) {
    Icon* icon = icon_from(resource);
    for (Held* h : icon->pictures)
        h->icon = nullptr;
    delete icon;
}

fs::path icon_dir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return fs::path(runtime ? runtime : "/tmp") / "atrium" / "icons";
}

// The largest picture, written out as a PNG the shell can show. Empty when
// there is none it can read.
std::string save_icon(const Icon& icon, uint64_t view_id) {
    const Held* best = nullptr;
    for (const Held* h : icon.pictures)
        if (!best || h->buffer->width > best->buffer->width)
            best = h;
    if (!best)
        return {};
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(best->buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride))
        return {};
    std::string path;
    if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888) {
        std::error_code ec;
        fs::create_directories(icon_dir(), ec);
        path = (icon_dir() / (std::to_string(view_id) + ".png")).string();
        cairo_surface_t* s = cairo_image_surface_create_for_data(
            static_cast<unsigned char*>(data), format == DRM_FORMAT_ARGB8888 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24,
            best->buffer->width, best->buffer->height, int(stride));
        if (cairo_surface_write_to_png(s, path.c_str()) != CAIRO_STATUS_SUCCESS)
            path.clear();
        cairo_surface_destroy(s);
    }
    wlr_buffer_end_data_ptr_access(best->buffer);
    return path;
}

} // namespace

ToplevelIcons::ToplevelIcons(Server& server) : server_(server) {
    global_ = wl_global_create(server.display, &xdg_toplevel_icon_manager_v1_interface, 1, this, &bind);
}

ToplevelIcons::~ToplevelIcons() {
    for (wl_resource* m : managers_) {
        wl_resource_set_user_data(m, nullptr);
        wl_resource_set_destructor(m, nullptr);
    }
    if (global_)
        wl_global_destroy(global_);
}

void ToplevelIcons::bind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    auto* self = static_cast<ToplevelIcons*>(data);
    wl_resource* r = wl_resource_create(client, &xdg_toplevel_icon_manager_v1_interface, int(version), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct xdg_toplevel_icon_manager_v1_interface impl = {
        .destroy = &destroy_resource,
        .create_icon = &create_icon,
        .set_icon = &set_icon,
    };
    wl_resource_set_implementation(r, &impl, self, &manager_gone);
    self->managers_.push_back(r);
    for (int size : {32, 48, 64, 128})
        xdg_toplevel_icon_manager_v1_send_icon_size(r, size);
    xdg_toplevel_icon_manager_v1_send_done(r);
}

void ToplevelIcons::manager_gone(wl_resource* resource) {
    if (auto* self = static_cast<ToplevelIcons*>(wl_resource_get_user_data(resource)))
        std::erase(self->managers_, resource);
}

void ToplevelIcons::create_icon(wl_client* client, wl_resource* manager, uint32_t id) {
    wl_resource* r = wl_resource_create(client, &xdg_toplevel_icon_v1_interface, wl_resource_get_version(manager), id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    static const struct xdg_toplevel_icon_v1_interface impl = {
        .destroy = &destroy_resource,
        .set_name = &icon_set_name,
        .add_buffer = &icon_add_buffer,
    };
    wl_resource_set_implementation(r, &impl, new Icon, &icon_gone);
}

void ToplevelIcons::set_icon(wl_client*, wl_resource* manager, wl_resource* toplevel_resource, wl_resource* icon_resource) {
    auto* self = static_cast<ToplevelIcons*>(wl_resource_get_user_data(manager));
    Icon* icon = icon_resource ? icon_from(icon_resource) : nullptr;
    if (icon)
        icon->immutable = true;
    wlr_xdg_toplevel* toplevel = wlr_xdg_toplevel_from_resource(toplevel_resource);
    View* v = (self && toplevel && toplevel->base) ? static_cast<View*>(toplevel->base->data) : nullptr;
    if (!v)
        return;
    std::error_code ec;
    fs::remove(icon_dir() / (std::to_string(v->id) + ".png"), ec);
    v->icon.clear();
    if (icon)
        v->icon = !icon->name.empty() ? icon->name : save_icon(*icon, v->id);
    if (v->mapped)
        self->server_.notify_window(*v, "changed");
}

} // namespace atrium
