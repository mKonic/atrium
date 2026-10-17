#pragma once
// Badges and progress apps put on their Dock icon, through the Unity launcher
// API that KDE Plasma's task manager and GNOME's Dash to Dock also take:
// apps broadcast com.canonical.Unity.LauncherEntry.Update(app_uri, props) on
// the session bus, where app_uri is "application://<desktop id>.desktop" and
// props holds only what changed: count (x), count-visible (b), progress (d,
// 0..1), progress-visible (b), urgent (b). Electron (app.setBadgeCount),
// Chromium (download progress), Telegram, Thunderbird use it. Electron only
// sends while the name com.canonical.Unity is owned, so the Dock owns it, as
// Plasma does. An app's entry goes when the bus connection that sent it does.

#include <QDBusContext>
#include <QHash>
#include <QObject>
#include <QVariantMap>

class QDBusServiceWatcher;

namespace atrium {

class LauncherEntries : public QObject, protected QDBusContext {
    Q_OBJECT

public:
    struct Entry {
        qint64 count = 0;
        bool countVisible = false;
        double progress = 0;  // 0..1
        bool progressVisible = false;
        bool urgent = false;
    };

    static LauncherEntries* instance();

    // By desktop entry id, without ".desktop"; empty when the app set nothing.
    Entry entry(const QString& id) const { return entries_.value(id); }

    // "application://firefox.desktop" (or a bare "firefox.desktop") → "firefox".
    static QString idForUri(const QString& uri);
    // Folds an Update's properties into what is known (it carries only changes).
    static void apply(Entry& entry, const QVariantMap& props);

signals:
    void changed(const QString& id);

public slots:
    void Update(const QString& uri, const QVariantMap& props);

private:
    LauncherEntries();
    void senderGone(const QString& service);

    QHash<QString, Entry> entries_;
    QHash<QString, QStringList> bySender_;  // bus name → ids it set
    QDBusServiceWatcher* watcher_ = nullptr;
};

} // namespace atrium
