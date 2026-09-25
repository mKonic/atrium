#pragma once
// The shell's windows. PanelWindow is a layer-shell surface (the bar, the
// Dock, panels, overlays) through LayerShellQt; FloatingWindow is an
// ordinary app window (System Settings). Both are Qt Quick windows whose
// children are the content, and both wait for their declaration to finish
// before appearing, so a panel is never shown before it knows its layer.

#include <QPointer>
#include <QQmlListProperty>
#include <QQmlParserStatus>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegion>
#include <QtQml/qqml.h>

#include "screens.hpp"

namespace LayerShellQt {
class Window;
}

namespace atrium::shell {

// An input region: `mask: Region { item: shelf }`, with child Regions
// added to it. An empty Region takes no input at all.
class Region : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickItem* item READ item WRITE setItem NOTIFY changed)
    Q_PROPERTY(QQmlListProperty<atrium::shell::Region> regions READ regions)
    Q_CLASSINFO("DefaultProperty", "regions")

public:
    using QObject::QObject;

    QQuickItem* item() const { return item_; }
    void setItem(QQuickItem* item);
    QQmlListProperty<Region> regions();

    // In the coordinates of the item's window.
    QRegion region() const;

signals:
    void changed();

private:
    static void append(QQmlListProperty<Region>* list, Region* r);
    static qsizetype count(QQmlListProperty<Region>* list);
    static Region* at(QQmlListProperty<Region>* list, qsizetype i);
    static void clear(QQmlListProperty<Region>* list);
    void watch(QQuickItem* item);

    QPointer<QQuickItem> item_;
    QList<QPointer<QQuickItem>> watched_;
    QList<Region*> regions_;
};

class ShellWindow : public QQuickWindow, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(bool visible READ wantsVisible WRITE setWantsVisible NOTIFY visibleChanged OVERRIDE)
    Q_PROPERTY(atrium::shell::ShellScreen* screen READ shellScreen WRITE setShellScreen NOTIFY shellScreenChanged)
    Q_PROPERTY(int implicitWidth READ implicitWidth WRITE setImplicitWidth NOTIFY implicitWidthChanged)
    Q_PROPERTY(int implicitHeight READ implicitHeight WRITE setImplicitHeight NOTIFY implicitHeightChanged)
    Q_PROPERTY(atrium::shell::Region* mask READ mask WRITE setMask NOTIFY maskChanged)

public:
    explicit ShellWindow(QWindow* parent = nullptr);

    bool wantsVisible() const { return visible_; }
    void setWantsVisible(bool visible);
    ShellScreen* shellScreen() const { return screen_; }
    void setShellScreen(ShellScreen* screen);
    int implicitWidth() const { return implicit_.width(); }
    void setImplicitWidth(int w);
    int implicitHeight() const { return implicit_.height(); }
    void setImplicitHeight(int h);
    Region* mask() const { return mask_; }
    void setMask(Region* mask);

    void classBegin() override {}
    void componentComplete() override;

signals:
    void shellScreenChanged();
    void implicitWidthChanged();
    void implicitHeightChanged();
    void maskChanged();

protected:
    // Before the window first shows (and again after a screen change).
    virtual void prepare() {}
    virtual void implicitSizeChanged();
    bool complete() const { return complete_; }
    QSize implicitSize() const { return implicit_; }

private:
    void applyVisible();
    void applyMask();

    bool complete_ = false;
    bool visible_ = true;
    QSize implicit_{0, 0};
    QPointer<ShellScreen> screen_;
    QPointer<Region> mask_;
};

class WlrLayer : public QObject {
    Q_OBJECT

public:
    enum Enum { Background, Bottom, Top, Overlay };
    Q_ENUM(Enum)
};

class WlrKeyboardFocus : public QObject {
    Q_OBJECT

public:
    enum Enum { None, Exclusive, OnDemand };
    Q_ENUM(Enum)
};

class ExclusionMode : public QObject {
    Q_OBJECT

public:
    // Normal: exactly exclusiveZone. Ignore: -1 (covers other panels' zones).
    // Auto: the window's own size on the edge it sits against, if any.
    enum Enum { Normal, Ignore, Auto };
    Q_ENUM(Enum)
};

// `WlrLayershell.layer`, `.namespace`, `.keyboardFocus` on a PanelWindow.
class LayerSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(int layer READ layer WRITE setLayer NOTIFY changed)
    Q_PROPERTY(QString namespace READ ns WRITE setNs NOTIFY changed)
    Q_PROPERTY(int keyboardFocus READ keyboardFocus WRITE setKeyboardFocus NOTIFY changed)

public:
    using QObject::QObject;

    int layer() const { return layer_; }
    void setLayer(int l);
    QString ns() const { return ns_; }
    void setNs(const QString& n);
    int keyboardFocus() const { return focus_; }
    void setKeyboardFocus(int f);

signals:
    void changed();

private:
    int layer_ = WlrLayer::Top;
    QString ns_ = QStringLiteral("atrium");
    int focus_ = WlrKeyboardFocus::None;
};

class WlrLayershell : public QObject {
    Q_OBJECT
    QML_ATTACHED(atrium::shell::LayerSettings)

public:
    static LayerSettings* qmlAttachedProperties(QObject* object);
};

class PanelEdges : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool top MEMBER top NOTIFY changed)
    Q_PROPERTY(bool bottom MEMBER bottom NOTIFY changed)
    Q_PROPERTY(bool left MEMBER left NOTIFY changed)
    Q_PROPERTY(bool right MEMBER right NOTIFY changed)

public:
    using QObject::QObject;
    bool top = false, bottom = false, left = false, right = false;

signals:
    void changed();
};

class PanelMargins : public QObject {
    Q_OBJECT
    Q_PROPERTY(int top MEMBER top NOTIFY changed)
    Q_PROPERTY(int bottom MEMBER bottom NOTIFY changed)
    Q_PROPERTY(int left MEMBER left NOTIFY changed)
    Q_PROPERTY(int right MEMBER right NOTIFY changed)

public:
    using QObject::QObject;
    int top = 0, bottom = 0, left = 0, right = 0;

signals:
    void changed();
};

class PanelWindow : public ShellWindow {
    Q_OBJECT
    Q_PROPERTY(atrium::shell::PanelEdges* anchors READ anchors CONSTANT)
    Q_PROPERTY(atrium::shell::PanelMargins* margins READ margins CONSTANT)
    Q_PROPERTY(int exclusiveZone READ exclusiveZone WRITE setExclusiveZone NOTIFY exclusiveZoneChanged)
    Q_PROPERTY(int exclusionMode READ exclusionMode WRITE setExclusionMode NOTIFY exclusiveZoneChanged)

public:
    explicit PanelWindow(QWindow* parent = nullptr);

    PanelEdges* anchors() { return &anchors_; }
    PanelMargins* margins() { return &margins_; }
    int exclusiveZone() const { return zone_; }
    void setExclusiveZone(int zone);
    int exclusionMode() const { return mode_; }
    void setExclusionMode(int mode);
    LayerSettings* layerSettings() { return &layer_; }

signals:
    void exclusiveZoneChanged();

protected:
    void prepare() override;
    void implicitSizeChanged() override;

private:
    void apply();

    PanelEdges anchors_;
    PanelMargins margins_;
    LayerSettings layer_;
    int zone_ = 0;
    int mode_ = ExclusionMode::Auto;
    LayerShellQt::Window* shell_ = nullptr;
};

class FloatingWindow : public ShellWindow {
    Q_OBJECT
    Q_PROPERTY(QSize minimumSize READ minimumSize WRITE setMinimumSize NOTIFY minimumSizeChanged)
    // False: no title bar from atrium; the window draws its own traffic
    // lights (in its sidebar, as macOS 26's apps do) and drag areas.
    Q_PROPERTY(bool titleBar READ titleBar WRITE setTitleBar NOTIFY titleBarChanged)
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)

public:
    explicit FloatingWindow(QWindow* parent = nullptr);

    bool titleBar() const { return !(flags() & Qt::FramelessWindowHint); }
    void setTitleBar(bool on);
    bool fullScreen() const { return windowStates() & Qt::WindowFullScreen; }

    // For a window's own title-bar controls.
    Q_INVOKABLE void startMove() { startSystemMove(); }
    Q_INVOKABLE void minimize() { showMinimized(); }
    Q_INVOKABLE void toggleZoom() { windowStates() & Qt::WindowMaximized ? showNormal() : showMaximized(); }
    Q_INVOKABLE void toggleFullScreen() { fullScreen() ? showNormal() : showFullScreen(); }

signals:
    void minimumSizeChanged();
    void titleBarChanged();
    void fullScreenChanged();

protected:
    void prepare() override;
    void implicitSizeChanged() override;
};

} // namespace atrium::shell
