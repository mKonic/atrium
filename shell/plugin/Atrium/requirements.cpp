#include "requirements.hpp"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QFileInfo>
#include <QStandardPaths>

namespace atrium {

namespace {

struct Service {
    const char* need;     // what a setting names in its `needs`
    const char* name;     // what to call it
    QStringList buses;    // any of these on the system bus
    bool activatable;     // started on first use, so installed is enough
};

const QList<Service>& services() {
    static const QList<Service> list = {
        {"networkmanager", "NetworkManager", {"org.freedesktop.NetworkManager"}, false},
        {"bluez", "Bluetooth (BlueZ)", {"org.bluez"}, false},
        {"power-profiles-daemon", "power-profiles-daemon",
         {"org.freedesktop.UPower.PowerProfiles", "net.hadess.PowerProfiles"}, true},
        {"accountsservice", "AccountsService", {"org.freedesktop.Accounts"}, true},
    };
    return list;
}

struct Program {
    const char* need;
    const char* binary;
};

constexpr Program kPrograms[] = {
    {"cliphist", "cliphist"},
    {"ddcutil", "ddcutil"},
    {"gpu-screen-recorder", "gpu-screen-recorder"},
};

} // namespace

Requirements* Requirements::instance() {
    static auto* self = new Requirements;
    return self;
}

Requirements::Requirements() {
    QDBusConnection::systemBus().connect("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                         "NameOwnerChanged", this,
                                         SLOT(nameOwnerChanged(QString, QString, QString)));
    check();
}

void Requirements::nameOwnerChanged(const QString& name, const QString&, const QString&) {
    for (const Service& s : services())
        if (s.buses.contains(name)) {
            check();
            return;
        }
}

void Requirements::check() {
    QVariantMap missing;
    QDBusConnectionInterface* bus = QDBusConnection::systemBus().interface();
    const QStringList running = bus ? bus->registeredServiceNames().value() : QStringList();
    const QStringList startable = bus ? bus->activatableServiceNames().value() : QStringList();
    for (const Service& s : services()) {
        bool present = false;
        for (const QString& b : s.buses)
            present = present || running.contains(b) || (s.activatable && startable.contains(b));
        if (!present)
            missing.insert(s.need, QStringLiteral("%1 isn't running.").arg(s.name));
    }
    for (const Program& p : kPrograms)
        if (QStandardPaths::findExecutable(p.binary).isEmpty())
            missing.insert(p.need, QStringLiteral("Needs %1, which isn't installed.").arg(p.binary));
    // No adapter: BlueZ doesn't even start then, and that's the thing to say.
    if (!QFileInfo::exists("/sys/class/bluetooth")) {
        missing.remove("bluez");
        missing.insert("bluetooth-adapter", QStringLiteral("This computer has no Bluetooth."));
    }
    // Sound: PipeWire's (or PulseAudio's) socket, which systemd holds open
    // even before the server starts.
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!QFileInfo::exists(runtime + "/pulse/native"))
        missing.insert("pipewire", QStringLiteral("PipeWire isn't running."));
    if (missing != missing_) {
        missing_ = missing;
        emit missingChanged();
    }
}

} // namespace atrium
