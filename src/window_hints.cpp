// What windows say about themselves beyond xdg-shell: an icon, a tag, that
// a dialog is modal, what kind of content they show and whether they would
// rather tear than wait for vblank. Plus the sandbox line for apps that
// arrive through security-context.
#include "output.hpp"
#include "sandbox.hpp"
#include "server.hpp"
#include "view.hpp"

#include <cairo.h>
#include <drm_fourcc.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace atrium {

namespace {

namespace fs = std::filesystem;

fs::path icon_dir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return fs::path(runtime ? runtime : "/tmp") / "atrium" / "icons";
}

// The largest picture the app sent, written out as a PNG the shell can show.
// Empty when there is none it can read.
std::string save_icon(const wlr_xdg_toplevel_icon_v1* icon, uint64_t view_id) {
    wlr_xdg_toplevel_icon_v1_buffer *b, *best = nullptr;
    wl_list_for_each(b, &icon->buffers, link)
        if (!best || b->buffer->width > best->buffer->width)
            best = b;
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

View* view_of(wlr_xdg_toplevel* toplevel) {
    return toplevel && toplevel->base ? static_cast<View*>(toplevel->base->data) : nullptr;
}

} // namespace

void Server::setup_window_hints() {
    wlr_xdg_wm_dialog_v1_create(display, 1);  // read through View::modal()

    auto* icons = wlr_xdg_toplevel_icon_manager_v1_create(display, 1);
    int sizes[] = {32, 48, 64, 128};
    wlr_xdg_toplevel_icon_manager_v1_set_sizes(icons, sizes, std::size(sizes));
    set_icon_.connect(&icons->events.set_icon, [this](wlr_xdg_toplevel_icon_manager_v1_set_icon_event* e) {
        View* v = view_of(e->toplevel);
        if (!v)
            return;
        std::error_code ec;
        fs::remove(icon_dir() / (std::to_string(v->id) + ".png"), ec);
        v->icon.clear();
        if (e->icon)
            v->icon = e->icon->name ? e->icon->name : save_icon(e->icon, v->id);
        if (v->mapped)
            notify_window(*v, "changed");
    });

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
        fs::remove(view.icon, ec);
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

} // namespace atrium
