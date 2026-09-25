#include "glass_shape.hpp"

#include "atrium-glass-v1-client-protocol.h"

#include <QGuiApplication>
#include <QHash>
#include <QQuickWindow>
#include <qpa/qplatformnativeinterface.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace atrium::shell {

namespace {

// The manager, bound once on a queue of our own (Qt's stays untouched).
atrium_glass_manager_v1* manager() {
    static atrium_glass_manager_v1* m = []() -> atrium_glass_manager_v1* {
        auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!app)
            return nullptr;
        wl_display* display = app->display();
        wl_event_queue* queue = wl_display_create_queue(display);
        auto* wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapped), queue);
        wl_registry* registry = wl_display_get_registry(wrapped);
        atrium_glass_manager_v1* found = nullptr;
        static const wl_registry_listener listener = {
            .global = [](void* data, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
                if (std::strcmp(iface, atrium_glass_manager_v1_interface.name) == 0)
                    *static_cast<atrium_glass_manager_v1**>(data) = static_cast<atrium_glass_manager_v1*>(
                        wl_registry_bind(r, name, &atrium_glass_manager_v1_interface, std::min(version, 2u)));
            },
            .global_remove = [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, &found);
        wl_display_roundtrip_queue(display, queue);
        if (found)
            wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(found), nullptr);
        wl_registry_destroy(registry);
        wl_proxy_wrapper_destroy(wrapped);
        wl_event_queue_destroy(queue);
        return found;
    }();
    return m;
}

// A window's glass: its shapes, sent while the scene syncs (the GUI thread
// waits then, so the items hold still), before the frame that shows them is
// committed.
class GlassSurface : public QObject {
public:
    explicit GlassSurface(QQuickWindow* window) : QObject(window), window_(window) {
        connect(window, &QQuickWindow::beforeSynchronizing, this, [this] { sync(); }, Qt::DirectConnection);
        // A hidden window's surface is gone, and a shown one is new.
        connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible)
                drop();
        });
    }
    ~GlassSurface() override { drop(); }

    static GlassSurface* of(QQuickWindow* window) {
        static QHash<QQuickWindow*, GlassSurface*> all;
        auto it = all.find(window);
        if (it != all.end())
            return *it;
        auto* s = new GlassSurface(window);
        all.insert(window, s);
        connect(window, &QObject::destroyed, [window] { all.remove(window); });
        return s;
    }

    void add(GlassShape* shape) {
        if (!shapes_.contains(shape))
            shapes_.push_back(shape);
    }
    void remove(GlassShape* shape) { shapes_.removeAll(shape); }

private:
    void sync() {
        if (!glass_) {
            atrium_glass_manager_v1* m = manager();
            auto* surface = static_cast<wl_surface*>(
                QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window_));
            if (!m || !surface)
                return;
            glass_ = atrium_glass_manager_v1_get_glass(m, surface);
            sent_.clear();
            sent_once_ = false;
        }
        // Version 2 takes a clip with each shape.
        const bool clipping = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(glass_)) >= 2;
        std::vector<wl_fixed_t> now;
        for (GlassShape* s : shapes_) {
            if (!s || !s->isVisible())
                continue;
            qreal opacity = 1;
            for (QQuickItem* i = s; i; i = i->parentItem())
                opacity *= i->opacity();
            // Fading out: gone before it's too faint to see, not left behind
            // at a hair above nothing.
            if (opacity < 0.01)
                continue;
            const QRectF whole = s->mapRectToScene(QRectF(0, 0, s->width(), s->height()));
            // Only what shows: a card scrolled out of a clipping list (the
            // Notification Center's) leaves its glass behind otherwise, past
            // the list's edge. atrium cuts it there (no rim along the cut);
            // one that can't gets it shrunk to what shows.
            QRectF shown = whole;
            bool clipped = false;
            for (QQuickItem* i = s->parentItem(); i; i = i->parentItem())
                if (i->clip()) {
                    shown &= i->mapRectToScene(i->clipRect());
                    clipped = true;
                }
            if (shown.width() <= 0 || shown.height() <= 0)
                continue;
            const qreal scale = s->width() > 0 ? whole.width() / s->width() : 1;
            const QRectF r = clipping ? whole : shown;
            for (qreal v : {r.x(), r.y(), r.width(), r.height(), s->radius() * scale, opacity})
                now.push_back(wl_fixed_from_double(v));
            if (clipping) {
                const QRectF c = clipped ? shown : QRectF();
                for (qreal v : {c.x(), c.y(), c.width(), c.height()})
                    now.push_back(wl_fixed_from_double(v));
            }
        }
        if (sent_once_ && now == sent_)
            return;
        wl_array array;
        wl_array_init(&array);
        if (!now.empty()) {
            void* data = wl_array_add(&array, now.size() * sizeof(wl_fixed_t));
            std::memcpy(data, now.data(), now.size() * sizeof(wl_fixed_t));
        }
        if (clipping)
            atrium_glass_v1_set_clipped_shapes(glass_, &array);
        else
            atrium_glass_v1_set_shapes(glass_, &array);
        wl_array_release(&array);
        sent_ = std::move(now);
        sent_once_ = true;
    }

    void drop() {
        if (glass_) {
            atrium_glass_v1_destroy(glass_);
            glass_ = nullptr;
        }
    }

    QQuickWindow* window_;
    QVector<QPointer<GlassShape>> shapes_;
    atrium_glass_v1* glass_ = nullptr;
    std::vector<wl_fixed_t> sent_;
    bool sent_once_ = false;
};

} // namespace

GlassShape::GlassShape(QQuickItem* parent) : QQuickItem(parent) {
    connect(this, &QQuickItem::visibleChanged, this, &GlassShape::changed);
    connect(this, &QQuickItem::opacityChanged, this, &GlassShape::changed);
}

GlassShape::~GlassShape() {
    attach(nullptr);
}

void GlassShape::setRadius(qreal r) {
    if (r == radius_)
        return;
    radius_ = r;
    emit radiusChanged();
    changed();
}

void GlassShape::itemChange(ItemChange change, const ItemChangeData& data) {
    if (change == ItemSceneChange)
        attach(data.window);
    QQuickItem::itemChange(change, data);
}

void GlassShape::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    changed();
}

void GlassShape::attach(QQuickWindow* window) {
    if (window_ == window)
        return;
    if (window_)
        GlassSurface::of(window_)->remove(this);
    window_ = window;
    if (window_)
        GlassSurface::of(window_)->add(this);
    changed();
}

// A frame, so the new shapes are sent (it draws nothing itself).
void GlassShape::changed() {
    if (window_)
        window_->update();
}

} // namespace atrium::shell
