#include "launcher_entries.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>

#include <cmath>

namespace atrium {

LauncherEntries* LauncherEntries::instance() {
    static LauncherEntries* self = new LauncherEntries;
    return self;
}

LauncherEntries::LauncherEntries() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect({}, {}, "com.canonical.Unity.LauncherEntry", "Update", this,
                SLOT(Update(QString, QVariantMap)));
    // Someone else (a second shell) may hold it already: then just listen.
    bus.registerService("com.canonical.Unity");
    watcher_ = new QDBusServiceWatcher(this);
    watcher_->setConnection(bus);
    watcher_->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(watcher_, &QDBusServiceWatcher::serviceUnregistered, this, &LauncherEntries::senderGone);
}

QString LauncherEntries::idForUri(const QString& uri) {
    QString id = uri;
    if (id.startsWith("application://"))
        id = id.mid(14);
    if (id.endsWith(".desktop"))
        id.chop(8);
    return id;
}

void LauncherEntries::apply(Entry& e, const QVariantMap& props) {
    if (auto it = props.find("count"); it != props.end())
        e.count = std::max<qint64>(0, it->toLongLong());
    if (auto it = props.find("count-visible"); it != props.end())
        e.countVisible = it->toBool();
    if (auto it = props.find("progress"); it != props.end()) {
        const double p = it->toDouble();
        e.progress = std::isfinite(p) ? std::clamp(p, 0.0, 1.0) : 0.0;
    }
    if (auto it = props.find("progress-visible"); it != props.end())
        e.progressVisible = it->toBool();
    if (auto it = props.find("urgent"); it != props.end())
        e.urgent = it->toBool();
}

void LauncherEntries::Update(const QString& uri, const QVariantMap& props) {
    const QString id = idForUri(uri);
    if (id.isEmpty())
        return;
    apply(entries_[id], props);
    if (calledFromDBus()) {
        const QString sender = message().service();
        QStringList& ids = bySender_[sender];
        if (!ids.contains(id))
            ids.push_back(id);
        watcher_->addWatchedService(sender);
    }
    emit changed(id);
}

void LauncherEntries::senderGone(const QString& service) {
    watcher_->removeWatchedService(service);
    for (const QString& id : bySender_.take(service)) {
        entries_.remove(id);
        emit changed(id);
    }
}

} // namespace atrium
