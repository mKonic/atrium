#include "phone_clipboard.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QVariantMap>

namespace atrium {

namespace {

const QString kService = QStringLiteral("org.atrium.ClipSync");
const QString kPath = QStringLiteral("/org/atrium/ClipSync");
const QString kInterface = QStringLiteral("org.atrium.ClipSync1");

} // namespace

PhoneClipboard::PhoneClipboard(QObject* parent)
    : QObject(parent),
      watcher_(new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(),
                                       QDBusServiceWatcher::WatchForRegistration |
                                           QDBusServiceWatcher::WatchForUnregistration,
                                       this)) {
    QDBusConnection::sessionBus().connect(kService, kPath, kInterface, QStringLiteral("StatusChanged"), this,
                                          SLOT(statusChanged(QString, QString)));
    connect(watcher_, &QDBusServiceWatcher::serviceRegistered, this, &PhoneClipboard::load);
    connect(watcher_, &QDBusServiceWatcher::serviceUnregistered, this, [this] { statusChanged({}, {}); });
    load();
}

QString PhoneClipboard::moduleUrl() const {
    return QStringLiteral("https://github.com/mKonic/atrium-clipsync-ksu/releases/latest/download/atrium-clipsync.zip");
}

void PhoneClipboard::load() {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                    QStringLiteral("GetAll"));
    m << kInterface;
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError())
            return;
        statusChanged(reply.value().value(QStringLiteral("State")).toString(),
                      reply.value().value(QStringLiteral("Phone")).toString());
    });
}

void PhoneClipboard::statusChanged(const QString& state, const QString& phone) {
    if (state == state_ && phone == phone_)
        return;
    state_ = state;
    phone_ = phone;
    emit changed();
}

} // namespace atrium
