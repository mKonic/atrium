#pragma once
// The focused app's own menus in the menu bar, as on macOS. A window says
// where its menus are on D-Bus (com.canonical.dbusmenu) through KDE's
// appmenu Wayland protocol (Qt apps), or an X11 app through the
// registrar (com.canonical.AppMenu.Registrar, by its window id), which
// atrium hosts: Qt only exports its menus when one is on the bus.

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QVariantList>

class QDBusServiceWatcher;

namespace atrium {

// com.canonical.AppMenu.Registrar: X11 windows' menus, by window id.
class AppMenuRegistrar : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.canonical.AppMenu.Registrar")

public:
    explicit AppMenuRegistrar(QObject* parent);
    std::pair<QString, QString> menuFor(uint window) const;
    // The last window a process registered: Qt 5 registers a Wayland
    // window's menus here too, under an id that names nothing, so its
    // process is all that ties them to its window.
    std::pair<QString, QString> menuForPid(uint pid) const;

public slots:
    Q_SCRIPTABLE void RegisterWindow(uint windowId, const QDBusObjectPath& menuObjectPath);
    Q_SCRIPTABLE void UnregisterWindow(uint windowId);
    Q_SCRIPTABLE QString GetMenuForWindow(uint windowId, QDBusObjectPath& menuObjectPath);

signals:
    Q_SCRIPTABLE void WindowRegistered(uint windowId, const QString& service, const QDBusObjectPath& menuObjectPath);
    Q_SCRIPTABLE void WindowUnregistered(uint windowId);
    void changed();

private:
    struct Registered {
        QString service, path;
        uint pid = 0;
        quint64 order = 0;
    };
    QHash<uint, Registered> windows_;  // by window id
    quint64 next_ = 1;
    QDBusServiceWatcher* watcher_ = nullptr;
};

class AppMenu : public QObject {
    Q_OBJECT
    // The focused window's menus: [{ id, text, enabled }]; none when it
    // shares none.
    Q_PROPERTY(QVariantList menus READ menus NOTIFY changed)

public:
    explicit AppMenu(QObject* parent = nullptr);

    QVariantList menus() const { return menus_; }
    // A menu's items, as dbusmenu::layout gives them (asked when it opens).
    Q_INVOKABLE QVariantList items(int id) const;
    Q_INVOKABLE void trigger(int id) const;

signals:
    void changed();

private slots:
    void layoutUpdated();

private:
    void follow();  // the focused window's menu, now
    void reload();

    AppMenuRegistrar* registrar_ = nullptr;
    QString service_, path_;
    QVariantList menus_;
    QTimer settle_;  // a burst of layout updates, one reload
};

} // namespace atrium
