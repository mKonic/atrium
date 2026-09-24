#include "shortcut_inhibitor.hpp"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>

#include <cstring>

namespace atrium::shell {

namespace {

// The manager, bound once on a queue of our own (Qt's stays untouched).
zwp_keyboard_shortcuts_inhibit_manager_v1* manager() {
    static zwp_keyboard_shortcuts_inhibit_manager_v1* m = [] () -> zwp_keyboard_shortcuts_inhibit_manager_v1* {
        auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!app)
            return nullptr;
        wl_display* display = app->display();
        wl_event_queue* queue = wl_display_create_queue(display);
        auto* wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), queue);
        wl_registry* registry = wl_display_get_registry(wrapped);
        zwp_keyboard_shortcuts_inhibit_manager_v1* found = nullptr;
        static const wl_registry_listener listener = {
            .global = [](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t) {
                if (std::strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0)
                    *static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1**>(data) =
                        static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                            wl_registry_bind(r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
            },
            .global_remove = [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, &found);
        wl_display_roundtrip_queue(display, queue);
        // Inhibitors made from it answer on Qt's queue, which is dispatched.
        if (found)
            wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(found), nullptr);
        wl_registry_destroy(registry);
        wl_proxy_wrapper_destroy(wrapped);
        wl_event_queue_destroy(queue);
        return found;
    }();
    return m;
}

const zwp_keyboard_shortcuts_inhibitor_v1_listener kListener = {
    .active = [](void* data, zwp_keyboard_shortcuts_inhibitor_v1*) {
        static_cast<ShortcutInhibitor*>(data)->setActive(true);
    },
    .inactive = [](void* data, zwp_keyboard_shortcuts_inhibitor_v1*) {
        static_cast<ShortcutInhibitor*>(data)->setActive(false);
    },
};

} // namespace

ShortcutInhibitor::~ShortcutInhibitor() {
    drop();
}

void ShortcutInhibitor::setWindow(QObject* window) {
    auto* w = qobject_cast<QWindow*>(window);
    if (w == window_)
        return;
    drop();
    if (window_)
        disconnect(window_, nullptr, this, nullptr);
    window_ = w;
    if (w) {
        // A hidden window's surface is gone, and a shown one is new.
        connect(w, &QWindow::visibleChanged, this, [this] {
            drop();
            update();
        });
    }
    emit windowChanged();
    update();
}

void ShortcutInhibitor::setEnabled(bool on) {
    if (on == enabled_)
        return;
    enabled_ = on;
    emit enabledChanged();
    if (!on)
        drop();
    update();
}

void ShortcutInhibitor::setActive(bool on) {
    if (on == active_)
        return;
    active_ = on;
    emit activeChanged();
}

void ShortcutInhibitor::update() {
    if (inhibitor_ || !enabled_ || !window_ || !window_->isVisible())
        return;
    auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    auto* surface = static_cast<wl_surface*>(
        QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window_));
    zwp_keyboard_shortcuts_inhibit_manager_v1* m = manager();
    if (!app || !surface || !m || !app->seat())
        return;
    inhibitor_ = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m, surface, app->seat());
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor_, &kListener, this);
}

void ShortcutInhibitor::drop() {
    if (inhibitor_) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor_);
        inhibitor_ = nullptr;
    }
    setActive(false);
}

} // namespace atrium::shell
