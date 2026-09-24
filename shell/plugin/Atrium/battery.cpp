#include "battery.hpp"

#include "battery_core.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
#include <QFile>
#include <QGuiApplication>

namespace atrium {

namespace {

const QString kUPower = QStringLiteral("org.freedesktop.UPower");
const QString kDevice = QStringLiteral("org.freedesktop.UPower.Device");
const QString kDisplay = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

QVariantMap getAll(const QString& service, const QString& path, const QString& interface) {
    QDBusMessage m = QDBusMessage::createMethodCall(service, path, kProps, "GetAll");
    m << interface;
    QDBusReply<QVariantMap> r = QDBusConnection::systemBus().call(m, QDBus::Block, 2000);
    return r.isValid() ? r.value() : QVariantMap();
}

battery::State state(uint s) {
    return static_cast<battery::State>(s);
}

} // namespace

Battery* Battery::instance() {
    static auto* self = new Battery;
    return self;
}

Battery::Battery() {
    findBattery();
    QDBusConnection bus = QDBusConnection::systemBus();
    for (const QString& path : {kDisplay, device_})
        if (!path.isEmpty())
            bus.connect(kUPower, path, kProps, "PropertiesChanged", this,
                        SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
    // The lid: only when there is one (logind reports a setting either way).
    if (QFile::exists("/proc/acpi/button/lid") || QFile::exists("/sys/class/input/lid")) {
        const QVariantMap m = getAll("org.freedesktop.login1", "/org/freedesktop/login1",
                                     "org.freedesktop.login1.Manager");
        lidAction_ = m.value("HandleLidSwitch").toString();
    }
    load();
}

void Battery::findBattery() {
    QDBusMessage m = QDBusMessage::createMethodCall(kUPower, "/org/freedesktop/UPower", kUPower, "EnumerateDevices");
    QDBusReply<QList<QDBusObjectPath>> r = QDBusConnection::systemBus().call(m, QDBus::Block, 2000);
    if (!r.isValid())
        return;
    for (const QDBusObjectPath& p : r.value()) {
        const QVariantMap d = getAll(kUPower, p.path(), kDevice);
        // A laptop's own battery (type 2), not a mouse's.
        if (d.value("Type").toUInt() == 2 && d.value("PowerSupply").toBool()) {
            device_ = p.path();
            return;
        }
    }
}

void Battery::load() {
    const QVariantMap d = getAll(kUPower, kDisplay, kDevice);
    present_ = !device_.isEmpty() && d.value("IsPresent").toBool();
    percent_ = d.value("Percentage").toDouble();
    state_ = d.value("State").toUInt();
    toEmpty_ = d.value("TimeToEmpty").toLongLong();
    toFull_ = d.value("TimeToFull").toLongLong();
    if (!device_.isEmpty()) {
        const QVariantMap b = getAll(kUPower, device_, kDevice);
        limitSupported_ = b.value("ChargeThresholdSupported").toBool();
        limitEnabled_ = b.value("ChargeThresholdEnabled").toBool();
        limitEnd_ = int(b.value("ChargeEndThreshold").toUInt());
    }
    if (present_)
        warn();
    emit changed();
}

void Battery::propertiesChanged(const QString& interface, const QVariantMap&, const QStringList&) {
    if (interface == kDevice)
        load();
}

bool Battery::plugged() const {
    return battery::plugged(state(state_));
}

QString Battery::glyph() const {
    return present_ ? QString::fromStdString(battery::glyph(percent_, state(state_))) : QString();
}

QString Battery::remaining() const {
    return QString::fromStdString(battery::remaining(state(state_), toEmpty_, toFull_));
}

void Battery::warn() {
    if (plugged())
        warned_ = 0;
    const int due = battery::warning(percent_, state(state_), warned_);
    // Said once, by the desktop shell (System Settings has a Battery too).
    if (!due || QGuiApplication::desktopFileName() != "atrium-shell")
        return;
    warned_ = due;
    QDBusMessage n = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                    "org.freedesktop.Notifications", "Notify");
    const QString title = due == 2 ? "Battery critically low" : "Battery low";
    const QString body = QString("%1% left. Plug in to keep working.").arg(percentage());
    n << QString("atrium") << uint(0) << QString("battery-caution") << title << body << QStringList()
      << QVariantMap{{"urgency", QVariant::fromValue(uchar(due == 2 ? 2 : 1))}} << -1;
    QDBusConnection::sessionBus().asyncCall(n);
}

void Battery::setChargeLimitEnabled(bool on) {
    if (device_.isEmpty())
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kUPower, device_, kDevice, "EnableChargeThreshold");
    m << on;
    m.setInteractiveAuthorizationAllowed(true);
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, 5 * 60 * 1000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        load();
    });
}

} // namespace atrium
