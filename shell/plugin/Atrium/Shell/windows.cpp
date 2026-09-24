#include "windows.hpp"

#include "screens.hpp"

#include <LayerShellQt/Window>

namespace atrium::shell {

// --- Region ------------------------------------------------------------------

void Region::setItem(QQuickItem* item) {
    if (item_ == item)
        return;
    for (const QPointer<QQuickItem>& w : watched_)
        if (w)
            disconnect(w, nullptr, this, nullptr);
    watched_.clear();
    item_ = item;
    // The item and everything above it: moving any of them moves the region.
    for (QQuickItem* i = item; i; i = i->parentItem())
        watch(i);
    emit changed();
}

void Region::watch(QQuickItem* item) {
    watched_.append(item);
    for (auto signal : {&QQuickItem::xChanged, &QQuickItem::yChanged, &QQuickItem::widthChanged,
                        &QQuickItem::heightChanged, &QQuickItem::visibleChanged})
        connect(item, signal, this, &Region::changed);
    connect(item, &QObject::destroyed, this, [this] { setItem(nullptr); });
}

QQmlListProperty<Region> Region::regions() {
    return {this, nullptr, &append, &count, &at, &clear};
}

void Region::append(QQmlListProperty<Region>* list, Region* r) {
    auto* self = static_cast<Region*>(list->object);
    if (!r)
        return;
    self->regions_.append(r);
    connect(r, &Region::changed, self, &Region::changed);
    emit self->changed();
}

qsizetype Region::count(QQmlListProperty<Region>* list) {
    return static_cast<Region*>(list->object)->regions_.size();
}

Region* Region::at(QQmlListProperty<Region>* list, qsizetype i) {
    return static_cast<Region*>(list->object)->regions_.at(i);
}

void Region::clear(QQmlListProperty<Region>* list) {
    auto* self = static_cast<Region*>(list->object);
    for (Region* r : self->regions_)
        disconnect(r, nullptr, self, nullptr);
    self->regions_.clear();
    emit self->changed();
}

QRegion Region::region() const {
    QRegion out;
    if (item_ && item_->isVisible()) {
        const QRectF r = item_->mapRectToScene(QRectF(0, 0, item_->width(), item_->height()));
        out += r.toAlignedRect();
    }
    for (const Region* r : regions_)
        out += r->region();
    return out;
}

// --- ShellWindow -------------------------------------------------------------

ShellWindow::ShellWindow(QWindow* parent) : QQuickWindow(parent) {
    setColor(Qt::white);
    // Hidden by the compositor (a layer surface closed): the property follows.
    connect(this, &QWindow::visibleChanged, this, [this](bool v) {
        if (complete_)
            visible_ = v;
    });
    connect(this, &QWindow::visibleChanged, this, [this](bool v) {
        if (v)
            applyMask();
    });
}

void ShellWindow::componentComplete() {
    complete_ = true;
    if (screen_ && screen_->screen())
        setScreen(screen_->screen());
    prepare();
    applyMask();
    applyVisible();
}

void ShellWindow::setWantsVisible(bool visible) {
    if (visible == visible_ && (!complete_ || isVisible() == visible))
        return;
    visible_ = visible;
    if (complete_)
        applyVisible();
}

void ShellWindow::applyVisible() {
    if (visible_ == isVisible())
        return;
    if (visible_) {
        prepare();
        QQuickWindow::setVisible(true);
    } else {
        QQuickWindow::setVisible(false);
    }
}

void ShellWindow::setShellScreen(ShellScreen* screen) {
    if (screen_ == screen)
        return;
    screen_ = screen;
    emit shellScreenChanged();
    if (!complete_ || !screen || !screen->screen() || this->screen() == screen->screen())
        return;
    // A surface stays on the output it was made for: make it anew there.
    const bool shown = isVisible();
    if (shown)
        QQuickWindow::setVisible(false);
    destroy();
    setScreen(screen->screen());
    if (shown) {
        prepare();
        QQuickWindow::setVisible(true);
    }
}

void ShellWindow::setImplicitWidth(int w) {
    if (w == implicit_.width())
        return;
    implicit_.setWidth(w);
    emit implicitWidthChanged();
    implicitSizeChanged();
}

void ShellWindow::setImplicitHeight(int h) {
    if (h == implicit_.height())
        return;
    implicit_.setHeight(h);
    emit implicitHeightChanged();
    implicitSizeChanged();
}

void ShellWindow::implicitSizeChanged() {
    if (implicit_.width() > 0 && implicit_.height() > 0)
        resize(implicit_);
}

void ShellWindow::setMask(Region* mask) {
    if (mask_ == mask)
        return;
    if (mask_)
        disconnect(mask_, nullptr, this, nullptr);
    mask_ = mask;
    if (mask)
        connect(mask, &Region::changed, this, &ShellWindow::applyMask);
    emit maskChanged();
    applyMask();
}

void ShellWindow::applyMask() {
    if (!mask_) {
        QWindow::setMask(QRegion());
        return;
    }
    QRegion r = mask_->region();
    // Qt reads an empty mask as "no mask"; a pixel outside the surface is
    // an input region that takes nothing.
    if (r.isEmpty())
        r = QRegion(-1, -1, 1, 1);
    QWindow::setMask(r);
}

// --- layer shell -------------------------------------------------------------

void LayerSettings::setLayer(int l) {
    if (l == layer_)
        return;
    layer_ = l;
    emit changed();
}

void LayerSettings::setNs(const QString& n) {
    if (n == ns_)
        return;
    ns_ = n;
    emit changed();
}

void LayerSettings::setKeyboardFocus(int f) {
    if (f == focus_)
        return;
    focus_ = f;
    emit changed();
}

LayerSettings* WlrLayershell::qmlAttachedProperties(QObject* object) {
    if (auto* w = qobject_cast<PanelWindow*>(object))
        return w->layerSettings();
    return new LayerSettings(object);
}

PanelWindow::PanelWindow(QWindow* parent) : ShellWindow(parent) {
    connect(&anchors_, &PanelEdges::changed, this, &PanelWindow::apply);
    connect(&margins_, &PanelMargins::changed, this, &PanelWindow::apply);
    connect(&layer_, &LayerSettings::changed, this, &PanelWindow::apply);
}

void PanelWindow::setExclusiveZone(int zone) {
    const bool same = zone == zone_ && mode_ == ExclusionMode::Normal;
    zone_ = zone;
    mode_ = ExclusionMode::Normal;
    if (same)
        return;
    emit exclusiveZoneChanged();
    apply();
}

void PanelWindow::setExclusionMode(int mode) {
    if (mode == mode_)
        return;
    mode_ = mode;
    emit exclusiveZoneChanged();
    apply();
}

void PanelWindow::prepare() {
    if (!shell_)
        shell_ = LayerShellQt::Window::get(this);
    apply();
}

void PanelWindow::implicitSizeChanged() {
    if (implicitSize().width() > 0 || implicitSize().height() > 0)
        resize(implicitSize().expandedTo({1, 1}));
    apply();
}

void PanelWindow::apply() {
    if (!shell_)
        return;
    using W = LayerShellQt::Window;
    W::Anchors a;
    if (anchors_.top)
        a |= W::AnchorTop;
    if (anchors_.bottom)
        a |= W::AnchorBottom;
    if (anchors_.left)
        a |= W::AnchorLeft;
    if (anchors_.right)
        a |= W::AnchorRight;
    shell_->setAnchors(a);
    shell_->setMargins({margins_.left, margins_.top, margins_.right, margins_.bottom});
    shell_->setLayer(W::Layer(layer_.layer()));
    shell_->setScope(layer_.ns());
    shell_->setKeyboardInteractivity(W::KeyboardInteractivity(layer_.keyboardFocus()));
    shell_->setDesiredSize(implicitSize());
    if (screen())
        shell_->setScreen(screen());

    int zone = zone_;
    if (mode_ == ExclusionMode::Ignore) {
        zone = -1;
    } else if (mode_ == ExclusionMode::Auto) {
        // Against one edge (a bar spanning it, or a panel in its middle):
        // the window's depth from that edge.
        const bool h = anchors_.left != anchors_.right, v = anchors_.top != anchors_.bottom;
        if (v && !h)
            zone = implicitSize().height();
        else if (h && !v)
            zone = implicitSize().width();
        else
            zone = 0;
    }
    shell_->setExclusiveZone(zone);
}

// --- FloatingWindow ----------------------------------------------------------

FloatingWindow::FloatingWindow(QWindow* parent) : ShellWindow(parent) {}

void FloatingWindow::prepare() {
    if (implicitSize().width() > 0 && implicitSize().height() > 0 && !isVisible())
        resize(implicitSize());
}

void FloatingWindow::implicitSizeChanged() {
    // Only the first size: after that the user's resizing wins.
    if (!isVisible())
        ShellWindow::implicitSizeChanged();
}

} // namespace atrium::shell
