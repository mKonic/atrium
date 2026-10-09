#include "foreign_parent.hpp"

#include "xdg-foreign-unstable-v2-client-protocol.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QSet>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>

#include <algorithm>
#include <cstring>

namespace atrium::shell {

namespace {

// The importer, bound once on a queue of our own (Qt's stays untouched).
zxdg_importer_v2* importer() {
    static zxdg_importer_v2* found = []() -> zxdg_importer_v2* {
        auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!app)
            return nullptr;
        wl_display* display = app->display();
        wl_event_queue* queue = wl_display_create_queue(display);
        auto* wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), queue);
        wl_registry* registry = wl_display_get_registry(wrapped);
        zxdg_importer_v2* bound = nullptr;
        static const wl_registry_listener listener = {
            .global = [](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t) {
                if (std::strcmp(iface, zxdg_importer_v2_interface.name) == 0)
                    *static_cast<zxdg_importer_v2**>(data) =
                        static_cast<zxdg_importer_v2*>(wl_registry_bind(r, name, &zxdg_importer_v2_interface, 1));
            },
            .global_remove = [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, &bound);
        wl_display_roundtrip_queue(display, queue);
        if (bound)
            wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(bound), nullptr);
        wl_registry_destroy(registry);
        wl_proxy_wrapper_destroy(wrapped);
        wl_event_queue_destroy(queue);
        return bound;
    }();
    return found;
}

class ForeignParent : public QObject {
public:
    explicit ForeignParent(QByteArray handle) : QObject(qGuiApp), handle_(std::move(handle)) {}

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::Expose)
            if (auto* w = qobject_cast<QWindow*>(o); w && w->isTopLevel() && w->isVisible() && !done_.contains(w))
                attach(w);
        return false;
    }

private:
    void attach(QWindow* window) {
        auto* surface = static_cast<wl_surface*>(
            QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window));
        zxdg_importer_v2* imp = importer();
        if (!surface || !imp)
            return;
        done_.insert(window);
        zxdg_imported_v2* imported = zxdg_importer_v2_import_toplevel(imp, handle_.constData());
        zxdg_imported_v2_set_parent_of(imported, surface);
        // Its parent for as long as it lives.
        connect(window, &QObject::destroyed, this, [this, window, imported] {
            done_.remove(window);
            zxdg_imported_v2_destroy(imported);
        });
        if (auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
            wl_display_flush(app->display());
    }

    QByteArray handle_;
    QSet<QWindow*> done_;
};

} // namespace

void installForeignParent() {
    static bool installed = false;
    const QByteArray parent = qgetenv("ATRIUM_PARENT_WINDOW");
    if (installed || !parent.startsWith("wayland:") || parent.size() <= 8)
        return;
    installed = true;
    qGuiApp->installEventFilter(new ForeignParent(parent.mid(8)));
}

} // namespace atrium::shell
