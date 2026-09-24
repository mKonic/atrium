#pragma once
// The system tray (StatusNotifierItem): `SystemTray.items`, the apps'
// icons for the bar, each with activate() and its menu (menu(), trigger()).
// atrium serves org.kde.StatusNotifierWatcher itself when nothing else
// does, and is the host that shows the items.

#include <QDBusContext>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QStringList>


namespace atrium {

class SystemTrayItem : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString icon READ icon NOTIFY changed)  // an image source
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool onlyMenu READ onlyMenu NOTIFY changed)
    Q_PROPERTY(bool hasMenu READ hasMenu NOTIFY changed)

public:
    SystemTrayItem(const QString& service, const QString& path, QObject* parent);
    ~SystemTrayItem() override;

    QString key() const { return service_ + path_; }
    QString id() const { return id_; }
    QString title() const { return title_; }
    QString icon() const { return icon_; }
    QString status() const { return status_; }
    bool onlyMenu() const { return onlyMenu_; }
    bool hasMenu() const { return !menu_.isEmpty() && menu_ != "/"; }

    Q_INVOKABLE void activate();
    Q_INVOKABLE void secondaryActivate();
    Q_INVOKABLE void scroll(int delta, bool horizontal);
    // The item's menu as the shell shows it: [{ id, text, checked, enabled,
    // separator, children }], hidden entries left out.
    Q_INVOKABLE QVariantList menu() const;
    // An entry of it was picked.
    Q_INVOKABLE void trigger(int id) const;

signals:
    void changed();

private slots:
    void refresh();

private:
    void apply(const QVariantMap& props);
    void call(const QString& method, const QVariantList& args);

    QString service_, path_;
    QString id_, title_, icon_, status_, menu_, iconName_, themePath_;
    bool onlyMenu_ = false;
    int serial_ = 0;
};

class TrayIcons : public QQuickImageProvider {
public:
    TrayIcons() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;
    static void put(const QString& key, const QImage& image);
    static void drop(const QString& key);
};

// org.kde.StatusNotifierWatcher, when atrium serves it.
class TrayWatcher : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierWatcher")
    Q_PROPERTY(QStringList RegisteredStatusNotifierItems READ items)
    Q_PROPERTY(bool IsStatusNotifierHostRegistered READ hostRegistered)
    Q_PROPERTY(int ProtocolVersion READ protocolVersion)

public:
    explicit TrayWatcher(QObject* parent);

    QStringList items() const { return items_; }
    bool hostRegistered() const { return true; }
    int protocolVersion() const { return 0; }

public slots:
    Q_SCRIPTABLE void RegisterStatusNotifierItem(const QString& serviceOrPath);
    Q_SCRIPTABLE void RegisterStatusNotifierHost(const QString& service);

signals:
    Q_SCRIPTABLE void StatusNotifierItemRegistered(const QString& item);
    Q_SCRIPTABLE void StatusNotifierItemUnregistered(const QString& item);
    Q_SCRIPTABLE void StatusNotifierHostRegistered();

private slots:
    void nameOwnerChanged(const QString& name, const QString& before, const QString& after);

private:
    QStringList items_;
};

class SystemTray : public QObject {
    Q_OBJECT
    // Items that want to be seen: Passive ones (nothing to show) are left out.
    Q_PROPERTY(QList<QObject*> items READ items NOTIFY itemsChanged)

public:
    static SystemTray* instance();

    QList<QObject*> items() const;

signals:
    void itemsChanged();

private slots:
    void registered(const QString& item);
    void unregistered(const QString& item);

private:
    SystemTray();
    void add(const QString& item);

    QPointer<TrayWatcher> watcher_;
    QList<SystemTrayItem*> items_;
    QList<QObject*> shown_;  // what items() last said
};

} // namespace atrium
