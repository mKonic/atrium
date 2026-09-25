#pragma once
// atrium's notification server (org.freedesktop.Notifications 1.2). Every
// notification goes into NotificationHistory; it pops up unless Do Not
// Disturb is on (critical ones always do). Icons and pictures are resolved
// here, so the shell's cards only show them.

#include <QAbstractListModel>
#include <QDBusContext>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QVariant>

namespace atrium {

class Notification : public QObject {
    Q_OBJECT
    Q_PROPERTY(uint id READ id CONSTANT)
    Q_PROPERTY(QString app READ app NOTIFY changed)          // the app's name to show
    Q_PROPERTY(QString icon READ icon NOTIFY changed)        // an image source, never empty
    Q_PROPERTY(QString image READ image NOTIFY changed)      // a picture (a photo, a cover), or ""
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString body READ body NOTIFY changed)
    Q_PROPERTY(bool critical READ critical NOTIFY changed)
    Q_PROPERTY(int timeout READ timeout NOTIFY changed)      // ms a popup stays
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)  // [{ id, text }], the default one aside
    Q_PROPERTY(bool hasDefault READ hasDefault NOTIFY changed)

public:
    struct Data {
        QString app, icon, image, summary, body, desktopEntry;
        int urgency = 1;
        int timeout = 5000;
        bool resident = false;
        QVariantList actions;
        bool hasDefault = false;
        bool transient = false;
        int uid = 0;  // its entry in the history
    };

    Notification(uint id, Data d, QObject* parent) : QObject(parent), id_(id), d_(std::move(d)) {}

    uint id() const { return id_; }
    QString app() const { return d_.app; }
    QString icon() const { return d_.icon; }
    QString image() const { return d_.image; }
    QString summary() const { return d_.summary; }
    QString body() const { return d_.body; }
    bool critical() const { return d_.urgency == 2; }
    int timeout() const { return d_.timeout; }
    QVariantList actions() const { return d_.actions; }
    bool hasDefault() const { return d_.hasDefault; }
    const Data& data() const { return d_; }
    void update(Data d) {
        d_ = std::move(d);
        emit changed();
    }

signals:
    void changed();
    // Closed by the app, or by the server for any reason.
    void closed();

private:
    uint id_;
    Data d_;
};

// Pictures sent as raw pixels, as "image://notification/ID".
class NotificationImages : public QQuickImageProvider {
public:
    NotificationImages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requested) override;
    static void put(const QString& key, const QImage& image);
    static void drop(const QString& key);
};

// The popups on screen, newest first, as a model that says which one came or
// went: a list property would rebuild every card, and all of them would
// slide in again together.
class PopupModel : public QAbstractListModel {
    Q_OBJECT

public:
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void prepend(Notification* n);
    bool remove(Notification* n);
    Notification* last() const;
    bool contains(Notification* n) const;
    int size() const { return int(items_.size()); }

private:
    QList<QPointer<Notification>> items_;
};

class NotificationServer : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
    Q_PROPERTY(QAbstractItemModel* popups READ popups CONSTANT)  // newest first, at most 5
    Q_PROPERTY(int popupCount READ popupCount NOTIFY popupsChanged)

public:
    static NotificationServer* instance();

    QAbstractItemModel* popups() const { return popups_; }
    int popupCount() const { return popups_->size(); }

    // The popup's time is up: it leaves the screen (and the history keeps it).
    Q_INVOKABLE void expire(atrium::Notification* n);
    // Swiped away or closed by the user.
    Q_INVOKABLE void dismiss(atrium::Notification* n);
    // Clicked: its default action, if it has one.
    Q_INVOKABLE void activate(atrium::Notification* n);
    Q_INVOKABLE void invoke(atrium::Notification* n, const QString& action);
    // The same, from its entry in the Notification Center: the app still
    // hears of it while it keeps the notification, else it is brought forward.
    Q_INVOKABLE void activateEntry(const QVariantMap& entry);
    Q_INVOKABLE void invokeEntry(const QVariantMap& entry, const QString& action);

    // --- D-Bus -------------------------------------------------------------
public slots:
    Q_SCRIPTABLE uint Notify(const QString& app_name, uint replaces_id, const QString& app_icon,
                             const QString& summary, const QString& body, const QStringList& actions,
                             const QVariantMap& hints, int expire_timeout);
    Q_SCRIPTABLE void CloseNotification(uint id);
    Q_SCRIPTABLE QStringList GetCapabilities();
    Q_SCRIPTABLE QString GetServerInformation(QString& vendor, QString& version, QString& spec_version);

signals:
    Q_SCRIPTABLE void NotificationClosed(uint id, uint reason);
    Q_SCRIPTABLE void ActionInvoked(uint id, const QString& action_key);
    void popupsChanged();

private:
    NotificationServer();
    Notification::Data resolve(const QString& app_name, const QString& app_icon, const QStringList& actions,
                               const QVariantMap& hints, int expire_timeout, uint id);
    void close(uint id, uint reason);
    Notification* byUid(int uid) const;

    QHash<uint, Notification*> live_;
    PopupModel* popups_ = new PopupModel(this);
    uint next_ = 1;
};

} // namespace atrium
