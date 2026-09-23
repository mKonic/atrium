#pragma once
// Notification history for the shell's notification center: what came in,
// newest first and grouped by app, kept across restarts in
// $XDG_STATE_HOME/atrium/notifications.json.

#include <QObject>
#include <QTimer>
#include <QVariant>

namespace atrium {

class NotificationHistory : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)    // newest first
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)  // [{ app, icon, items }], newest group first
    Q_PROPERTY(int unread READ unread NOTIFY changed)

public:
    explicit NotificationHistory(QObject* parent = nullptr);
    ~NotificationHistory() override;

    QVariantList items() const { return items_; }
    QVariantList groups() const;
    int unread() const { return unread_; }

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

private:
    void load();
    void save() const;
    void scheduleSave();

    QString file_;
    QVariantList items_;
    int unread_ = 0;
    int nextUid_ = 1;
    QTimer saveTimer_;
};

} // namespace atrium
