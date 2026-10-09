#include "wayland_link.hpp"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <wayland-client.h>

#include <algorithm>
#include <cstring>

namespace atrium {

namespace {

const wl_registry_listener kRegistry = {
    .global = [](void* data, wl_registry*, uint32_t name, const char* interface, uint32_t version) {
        static_cast<WaylandLink*>(data)->add_global(name, interface, version);
    },
    .global_remove = [](void* data, wl_registry*, uint32_t name) {
        static_cast<WaylandLink*>(data)->remove_global(name);
    },
};

const wl_output_listener kOutput = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
    .mode = [](void* data, wl_output*, uint32_t flags, int32_t, int32_t, int32_t refresh) {
        if (flags & WL_OUTPUT_MODE_CURRENT)
            static_cast<WaylandLink::Output*>(data)->refresh_mhz = refresh;
    },
    .done = [](void*, wl_output*) {},
    .scale = [](void*, wl_output*, int32_t) {},
    .name = [](void* data, wl_output*, const char* name) {
        static_cast<WaylandLink::Output*>(data)->name = QString::fromUtf8(name);
    },
    .description = [](void* data, wl_output*, const char* text) {
        static_cast<WaylandLink::Output*>(data)->description = QString::fromUtf8(text);
    },
};

const zxdg_output_v1_listener kXdgOutput = {
    .logical_position = [](void* data, zxdg_output_v1*, int32_t x, int32_t y) {
        auto* o = static_cast<WaylandLink::Output*>(data);
        o->x = x;
        o->y = y;
    },
    .logical_size = [](void* data, zxdg_output_v1*, int32_t w, int32_t h) {
        auto* o = static_cast<WaylandLink::Output*>(data);
        o->width = w;
        o->height = h;
    },
    .done = [](void*, zxdg_output_v1*) {},
    .name = [](void*, zxdg_output_v1*, const char*) {},
    .description = [](void*, zxdg_output_v1*, const char*) {},
};

struct ToplevelData {
    WaylandLink* link;
    WaylandLink::Toplevel* toplevel;
};

const ext_foreign_toplevel_handle_v1_listener kToplevel = {
    .closed = [](void* data, ext_foreign_toplevel_handle_v1*) {
        auto* d = static_cast<ToplevelData*>(data);
        d->link->remove_toplevel(d->toplevel);
        delete d;
    },
    .done = [](void*, ext_foreign_toplevel_handle_v1*) {},
    .title = [](void* data, ext_foreign_toplevel_handle_v1*, const char* title) {
        static_cast<ToplevelData*>(data)->toplevel->title = QString::fromUtf8(title);
    },
    .app_id = [](void* data, ext_foreign_toplevel_handle_v1*, const char* app_id) {
        static_cast<ToplevelData*>(data)->toplevel->app_id = QString::fromUtf8(app_id);
    },
    .identifier = [](void* data, ext_foreign_toplevel_handle_v1*, const char* id) {
        static_cast<ToplevelData*>(data)->toplevel->identifier = QString::fromUtf8(id);
    },
};

const ext_foreign_toplevel_list_v1_listener kList = {
    .toplevel = [](void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* handle) {
        static_cast<WaylandLink*>(data)->add_toplevel(handle);
    },
    .finished = [](void*, ext_foreign_toplevel_list_v1*) {},
};

} // namespace

WaylandLink* WaylandLink::instance() {
    static WaylandLink* self = [] {
        auto* link = new WaylandLink;
        if (!link->connect_display()) {
            delete link;
            return static_cast<WaylandLink*>(nullptr);
        }
        return link;
    }();
    return self;
}

WaylandLink::~WaylandLink() {
    if (display_)
        wl_display_disconnect(display_);
}

bool WaylandLink::connect_display() {
    display_ = wl_display_connect(nullptr);
    if (!display_)
        return false;
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistry, this);
    wl_display_roundtrip(display_);  // the globals
    wl_display_roundtrip(display_);  // what they say first: names, places, windows
    notifier_ = new QSocketNotifier(wl_display_get_fd(display_), QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &WaylandLink::dispatch);
    // Requests go out before Qt's loop sleeps.
    connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this,
            [this] { wl_display_flush(display_); });
    return true;
}

void WaylandLink::dispatch() {
    if (wl_display_dispatch(display_) < 0) {
        // The compositor went away: nothing more to share.
        notifier_->setEnabled(false);
        QCoreApplication::exit(1);
    }
}

void WaylandLink::roundtrip() {
    wl_display_roundtrip(display_);
}

void WaylandLink::add_global(uint32_t name, const char* interface, uint32_t version) {
    auto is = [interface](const wl_interface& i) { return std::strcmp(interface, i.name) == 0; };
    if (is(wl_output_interface)) {
        auto o = std::make_unique<Output>();
        o->global = name;
        o->wl = static_cast<wl_output*>(
            wl_registry_bind(registry_, name, &wl_output_interface, std::min(version, 4u)));
        wl_output_add_listener(o->wl, &kOutput, o.get());
        watch_output(o.get());
        outputs_.push_back(std::move(o));
    } else if (is(zxdg_output_manager_v1_interface)) {
        xdg_outputs_ = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry_, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
        for (auto& o : outputs_)
            watch_output(o.get());
    } else if (is(wl_shm_interface)) {
        shm = static_cast<wl_shm*>(wl_registry_bind(registry_, name, &wl_shm_interface, 1));
    } else if (is(zwp_linux_dmabuf_v1_interface) && version >= 3) {
        dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry_, name, &zwp_linux_dmabuf_v1_interface, 3));
    } else if (is(ext_image_copy_capture_manager_v1_interface)) {
        copy_manager = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry_, name, &ext_image_copy_capture_manager_v1_interface, 1));
    } else if (is(ext_output_image_capture_source_manager_v1_interface)) {
        output_sources = static_cast<ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(registry_, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    } else if (is(ext_foreign_toplevel_image_capture_source_manager_v1_interface)) {
        toplevel_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(wl_registry_bind(
            registry_, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
    } else if (is(ext_foreign_toplevel_list_v1_interface)) {
        list_ = static_cast<ext_foreign_toplevel_list_v1*>(
            wl_registry_bind(registry_, name, &ext_foreign_toplevel_list_v1_interface, 1));
        ext_foreign_toplevel_list_v1_add_listener(list_, &kList, this);
    }
}

void WaylandLink::watch_output(Output* o) {
    if (!xdg_outputs_ || o->xdg)
        return;
    o->xdg = zxdg_output_manager_v1_get_xdg_output(xdg_outputs_, o->wl);
    zxdg_output_v1_add_listener(o->xdg, &kXdgOutput, o);
}

void WaylandLink::remove_global(uint32_t name) {
    std::erase_if(outputs_, [name](const std::unique_ptr<Output>& o) {
        if (o->global != name)
            return false;
        if (o->xdg)
            zxdg_output_v1_destroy(o->xdg);
        wl_output_release(o->wl);
        return true;
    });
}

void WaylandLink::add_toplevel(ext_foreign_toplevel_handle_v1* handle) {
    auto t = std::make_unique<Toplevel>();
    t->handle = handle;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &kToplevel, new ToplevelData{this, t.get()});
    toplevels_.push_back(std::move(t));
}

void WaylandLink::remove_toplevel(Toplevel* t) {
    ext_foreign_toplevel_handle_v1_destroy(t->handle);
    std::erase_if(toplevels_, [t](const std::unique_ptr<Toplevel>& p) { return p.get() == t; });
}

WaylandLink::Output* WaylandLink::output(const QString& name) const {
    for (const auto& o : outputs_)
        if (o->name == name)
            return o.get();
    return nullptr;
}

WaylandLink::Toplevel* WaylandLink::toplevel(const QString& identifier) const {
    for (const auto& t : toplevels_)
        if (t->identifier == identifier)
            return t.get();
    return nullptr;
}

} // namespace atrium
