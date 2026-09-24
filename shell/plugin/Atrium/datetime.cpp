#include "datetime.hpp"

#include <QCollator>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>

#include <algorithm>
#include <ctime>

namespace atrium {

namespace {

const QString kService = QStringLiteral("org.freedesktop.timedate1");
const QString kPath = QStringLiteral("/org/freedesktop/timedate1");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

} // namespace

DateTime::DateTime(QObject* parent) : QObject(parent) {
    QDBusConnection::systemBus().connect(kService, kPath, kProps, "PropertiesChanged", this,
                                         SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
    load();
}

void DateTime::load() {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kProps, "GetAll");
    m << kService;
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<QVariantMap> r = *w;
        available_ = !r.isError();
        if (available_) {
            const QVariantMap p = r.value();
            const QString zone = p.value("Timezone").toString();
            // A new zone: glibc never reads /etc/localtime again by itself
            // (tzset() sees the same default name), so this process's clocks
            // are told through TZ, which apps started from here inherit.
            if (!timezone_.isEmpty() && zone != timezone_) {
                qputenv("TZ", (":" + zone).toUtf8());
                tzset();
            }
            timezone_ = zone;
            ntp_ = p.value("NTP").toBool();
            canNtp_ = p.value("CanNTP").toBool();
        }
        emit changed();
    });
}

void DateTime::propertiesChanged(const QString& interface, const QVariantMap&, const QStringList&) {
    if (interface == kService)
        load();  // timedated sends most of them as invalidated only
}

QString DateTime::label(const QString& zone) {
    // "America/Argentina/Buenos_Aires" → "Buenos Aires (America, Argentina)"
    QStringList parts = zone.split('/');
    if (parts.size() < 2)
        return QString(zone).replace('_', ' ');
    const QString city = parts.takeLast().replace('_', ' ');
    return city + " (" + parts.join(", ").replace('_', ' ') + ")";
}

QVariantList DateTime::findTimezones(const QString& query) {
    if (zones_.isEmpty()) {
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kService, "ListTimezones");
        QDBusReply<QStringList> r = QDBusConnection::systemBus().call(m, QDBus::Block, 3000);
        if (r.isValid())
            zones_ = r.value();
    }
    const QString q = query.trimmed();
    QVariantList out;
    for (const QString& z : zones_) {
        const QString l = label(z);
        if (q.isEmpty() || l.contains(q, Qt::CaseInsensitive) || z.contains(q, Qt::CaseInsensitive))
            out.append(QVariantMap{{"value", z}, {"label", l}});
    }
    QCollator order;
    std::sort(out.begin(), out.end(), [&](const QVariant& a, const QVariant& b) {
        return order.compare(a.toMap().value("label").toString(), b.toMap().value("label").toString()) < 0;
    });
    return out;
}

void DateTime::call(const QString& method, const QVariantList& args) {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kService, method);
    m.setArguments(args);
    // Or polkit refuses outright instead of asking for a password.
    m.setInteractiveAuthorizationAllowed(true);
    // Long enough for someone to type a password.
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 5 * 60 * 1000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<> r = *w;
        if (r.isError())
            emit failed(r.error().message());
        load();
    });
}

void DateTime::setTimezone(const QString& zone) {
    call("SetTimezone", {zone, true});
}

void DateTime::setNtp(bool on) {
    call("SetNTP", {on, true});
}

} // namespace atrium
