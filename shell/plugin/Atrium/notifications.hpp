#pragma once
// Notification history for the shell's notification center: what came in,
// newest first and grouped by app, kept across restarts in
// $XDG_STATE_HOME/atrium/notifications.json.

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <QVariant>

namespace atrium {

class NotificationHistory : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)    // newest first
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)  // [{ app, icon, items }], newest group first
    Q_PROPERTY(int unread READ unread NOTIFY changed)
    // Every app that has notified, with what its notifications do:
    // [{name, icon, mode}], mode "on", "quiet" (no popups) or "off".
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)

public:
    explicit NotificationHistory(QObject* parent = nullptr);
    // The one the notification server adds to and the shell shows.
    static NotificationHistory* instance();
    ~NotificationHistory() override;

    QVariantList items() const { return items_; }
    QVariantList groups() const;
    int unread() const { return unread_; }
    QVariantList apps() const;

    // "on", "quiet" or "off", kept in the notifications.quiet and .off settings.
    Q_INVOKABLE void setAppMode(const QString& app, const QString& mode);
    static QString modeOf(const QString& app);

    // { app, icon, summary, body, image, urgency, desktopEntry }; gets a uid and time.
    Q_INVOKABLE int add(const QVariantMap& entry);
    Q_INVOKABLE void remove(int uid);
    Q_INVOKABLE void clearApp(const QString& app);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void markRead();

    // "now", "5m", "2h", "Yesterday", "Mon 3 Aug": how long ago, shortly.
    Q_INVOKABLE QString ago(qint64 ms) const;

signals:
    void changed();
    void appsChanged();

private:
    void load();
    void save() const;
    void scheduleSave();

    QString file_;
    QVariantList items_;
    QVariantMap seen_;  // app name → icon, kept when the history is cleared
    int unread_ = 0;
    int nextUid_ = 1;
    QTimer saveTimer_;
    QFileSystemWatcher watcher_;
};

} // namespace atrium
