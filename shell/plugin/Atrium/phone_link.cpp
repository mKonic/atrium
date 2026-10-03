#include "phone_link.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QVariantMap>

namespace atrium {

namespace {

const QString kService = QStringLiteral("org.atrium.PhoneLink");
const QString kPath = QStringLiteral("/org/atrium/PhoneLink");
const QString kInterface = QStringLiteral("org.atrium.PhoneLink1");

// aa{sv} comes as a QDBusArgument.
QVariantList maps(const QVariant& v) {
    QVariantList out;
    if (!v.canConvert<QDBusArgument>())
        return out;
    const QDBusArgument arg = v.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QVariantMap m;
        arg >> m;
        out.push_back(m);
    }
    arg.endArray();
    return out;
}

} // namespace

PhoneLink::PhoneLink(QObject* parent)
    : QObject(parent),
      watcher_(new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(),
                                       QDBusServiceWatcher::WatchForRegistration |
                                           QDBusServiceWatcher::WatchForUnregistration,
                                       this)) {
    QDBusConnection::sessionBus().connect(kService, kPath, kInterface, QStringLiteral("StatusChanged"), this,
                                          SLOT(load()));
    connect(watcher_, &QDBusServiceWatcher::serviceRegistered, this, &PhoneLink::load);
    connect(watcher_, &QDBusServiceWatcher::serviceUnregistered, this, &PhoneLink::clear);
    load();
}

void PhoneLink::load() {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                    QStringLiteral("GetAll"));
    m << kInterface;
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError())
            return;
        const QVariantMap p = reply.value();
        state_ = p.value(QStringLiteral("State")).toString();
        phone_ = p.value(QStringLiteral("Phone")).toString();
        code_ = p.value(QStringLiteral("Code")).toString();
        phones_ = maps(p.value(QStringLiteral("Phones")));
        nearby_ = maps(p.value(QStringLiteral("Nearby")));
        emit changed();
    });
}

void PhoneLink::clear() {
    state_.clear();
    phone_.clear();
    code_.clear();
    phones_.clear();
    nearby_.clear();
    emit changed();
}

void PhoneLink::call(const QString& method, const QVariantList& args) {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
    m.setArguments(args);
    QDBusConnection::sessionBus().asyncCall(m);
}

} // namespace atrium
