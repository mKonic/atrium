#include "toplevel_icon.hpp"

#include "wl/buffer.hpp"

#include "server.hpp"
#include "view.hpp"

#include "xdg-toplevel-icon-v1-server.hpp"

#include <cairo.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace atrium {

namespace {

namespace fs = std::filesystem;

using wl::XdgToplevelIconManagerV1;
using wl::XdgToplevelIconV1;

struct Icon;

// A picture an app gave an icon, held (locked) from add_buffer until the app
// destroys the buffer: dropping the last lock earlier sends release.
struct Held {
    wlr_buffer* buffer = nullptr;
    int scale = 1;
    Icon* icon = nullptr;  // null once the icon is gone or replaced it
    wl_listener destroy{};
};

struct Icon : XdgToplevelIconV1 {
    Icon(wl_client* client, uint32_t version, uint32_t id) : XdgToplevelIconV1(client, version, id) {
        on_set_name([this](XdgToplevelIconV1*, const char* n) {
            if (mutable_())
                name = n;
        });
        on_add_buffer([this](XdgToplevelIconV1*, wl_resource* buffer, int32_t scale) { add_buffer(buffer, scale); });
    }
    ~Icon() override {
        for (Held* h : pictures)
            h->icon = nullptr;
    }

    bool mutable_() {
        if (immutable)
            post_error(uint32_t(Error::Immutable), "the icon was already assigned to a toplevel");
        return !immutable;
    }
    void add_buffer(wl_resource* buffer_resource, int32_t scale);

    std::string name;
    std::vector<Held*> pictures;
    bool immutable = false;  // assigned to a toplevel
};

void buffer_gone(wl_listener* listener, void*) {
    Held* h = wl_container_of(listener, h, destroy);
    wl_list_remove(&h->destroy.link);
    if (h->icon)
        std::erase(h->icon->pictures, h);
    wlr_buffer_unlock(h->buffer);  // the resource is gone: nothing is sent
    delete h;
}

void Icon::add_buffer(wl_resource* buffer_resource, int32_t scale) {
    if (!mutable_())
        return;
    wl::ClientBuffer* cb = wl::ClientBuffer::from(buffer_resource);
    wlr_buffer* buffer = cb ? wlr_buffer_lock(cb->buffer()) : nullptr;
    wlr_shm_attributes shm{};
    if (!buffer || !wlr_buffer_get_shm(buffer, &shm) || buffer->width != buffer->height) {
        if (buffer)
            wlr_buffer_unlock(buffer);
        post_error(uint32_t(Error::InvalidBuffer), "icon buffers must be square and backed by wl_shm");
        return;
    }
    // The same size and scale again replaces the picture; the old buffer
    // stays held until the app destroys it.
    for (Held*& h : pictures)
        if (h->buffer->width == buffer->width && h->scale == scale) {
            h->icon = nullptr;
            h = nullptr;
        }
    std::erase(pictures, nullptr);
    auto* h = new Held{.buffer = buffer, .scale = scale, .icon = this};
    h->destroy.notify = buffer_gone;
    wl_resource_add_destroy_listener(buffer_resource, &h->destroy);
    pictures.push_back(h);
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
    global_ = wl::Global::create<XdgToplevelIconManagerV1>(server.display, 1,
        [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* m = wl::make<XdgToplevelIconManagerV1>(client, version, id);
            if (!m)
                return;
            // Icons live on their own: they need nothing of ours.
            m->on_create_icon([](XdgToplevelIconManagerV1* self, uint32_t id) {
                wl::make<Icon>(self->client(), self->version(), id);
            });
            m->on_set_icon([this](XdgToplevelIconManagerV1*, wl_resource* toplevel, XdgToplevelIconV1* icon) {
                set_icon(toplevel, icon);
            });
            std::erase_if(managers_, [](const auto& w) { return !w; });
            managers_.push_back(m);
            for (int size : {32, 48, 64, 128})
                m->send_icon_size(size);
            m->send_done();
        });
}

ToplevelIcons::~ToplevelIcons() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
}

void ToplevelIcons::set_icon(wl_resource* toplevel_resource, XdgToplevelIconV1* icon_resource) {
    // Only an inert icon (made after we went) is not one of ours.
    auto* icon = dynamic_cast<Icon*>(icon_resource);
    if (icon)
        icon->immutable = true;
    wl::Toplevel* toplevel = wl::Toplevel::from(toplevel_resource);
    View* v = toplevel ? static_cast<View*>(toplevel->data) : nullptr;
    if (!v)
        return;
    std::error_code ec;
    fs::remove(icon_dir() / (std::to_string(v->id) + ".png"), ec);
    v->icon.clear();
    if (icon)
        v->icon = !icon->name.empty() ? icon->name : save_icon(*icon, v->id);
    if (v->mapped)
        server_.notify_window(*v, "changed");
}

} // namespace atrium
