#include "phone_link.hpp"

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
        error_ = p.value(QStringLiteral("Error")).toString();
        const QStringList ids = p.value(QStringLiteral("PhoneIds")).toStringList();
        const QStringList names = p.value(QStringLiteral("Phones")).toStringList();
        phones_.clear();
        for (qsizetype i = 0; i < ids.size() && i < names.size(); ++i)
            phones_.push_back(QVariantMap{{QStringLiteral("id"), ids[i]}, {QStringLiteral("name"), names[i]}});
        emit changed();
    });
}

void PhoneLink::clear() {
    state_.clear();
    phone_.clear();
    code_.clear();
    error_.clear();
    phones_.clear();
    emit changed();
}

void PhoneLink::call(const QString& method, const QString& arg) {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
    if (!arg.isNull())
        m << arg;
    QDBusConnection::sessionBus().asyncCall(m);
}

} // namespace atrium
