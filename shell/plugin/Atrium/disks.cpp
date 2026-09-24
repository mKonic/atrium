#include "disks.hpp"

#include "compositor.hpp"

#include <QCollator>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QLocale>
#include <QProcess>

#include <algorithm>

namespace atrium {

namespace {

const QString kUDisks = QStringLiteral("org.freedesktop.UDisks2");
const QString kBlock = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString kFilesystem = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString kDrive = QStringLiteral("org.freedesktop.UDisks2.Drive");

QDBusConnection bus() {
    return QDBusConnection::systemBus();
}

QMap<QString, QVariantMap> interfacesOf(const QDBusArgument& arg) {
    QMap<QString, QVariantMap> out;
    arg.beginMap();
    while (!arg.atEnd()) {
        QString name;
        QVariantMap props;
        arg.beginMapEntry();
        arg >> name >> props;
        arg.endMapEntry();
        out.insert(name, props);
    }
    arg.endMap();
    return out;
}

// udisks' byte strings end in a NUL.
QString bytes(const QVariant& v) {
    QByteArray b = v.toByteArray();
    if (b.endsWith('\0'))
        b.chop(1);
    return QString::fromUtf8(b);
}

QStringList mountPoints(const QVariant& v) {
    QStringList out;
    if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QByteArray b;
            arg >> b;
            if (b.endsWith('\0'))
                b.chop(1);
            out.append(QString::fromUtf8(b));
        }
        arg.endArray();
    }
    return out;
}

} // namespace

Disks* Disks::instance() {
    static auto* self = new Disks;
    return self;
}

Disks::Disks() {
    reload_.setSingleShot(true);
    reload_.setInterval(150);
    connect(&reload_, &QTimer::timeout, this, &Disks::reload);
    bus().connect(kUDisks, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this,
                  SLOT(reloadSoon()));
    bus().connect(kUDisks, "/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                  this, SLOT(reloadSoon()));
    // Any object's properties: mounts come and go through them.
    bus().connect(kUDisks, QString(), "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                  SLOT(reloadSoon()));
    QDBusConnection::sessionBus().connect("", "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                          "ActionInvoked", this, SLOT(actionInvoked(uint, QString)));
    reload();
}

void Disks::reload() {
    QDBusMessage m = QDBusMessage::createMethodCall(kUDisks, "/org/freedesktop/UDisks2",
                                                    "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        available_ = reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty();
        if (!available_) {
            disks_.clear();
            emit changed();
            return;
        }
        QMap<QString, QMap<QString, QVariantMap>> objects;
        const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
        arg.beginMap();
        while (!arg.atEnd()) {
            QDBusObjectPath path;
            arg.beginMapEntry();
            arg >> path;
            objects.insert(path.path(), interfacesOf(arg));
            arg.endMapEntry();
        }
        arg.endMap();

        QVariantList out;
        QSet<QString> now;
        const bool autoMount = Compositor::instance()->setting("disks.automount", true).toBool() &&
                               QGuiApplication::desktopFileName() == "atrium-shell";
        for (auto it = objects.begin(); it != objects.end(); ++it) {
            const QVariantMap block = it->value(kBlock);
            if (!it->contains(kFilesystem) || block.value("IdUsage").toString() != "filesystem" ||
                block.value("HintIgnore").toBool())
                continue;
            const QVariantMap drive = objects.value(block.value("Drive").value<QDBusObjectPath>().path()).value(kDrive);
            const bool removable = drive.value("Removable").toBool() || drive.value("MediaRemovable").toBool() ||
                                   drive.value("ConnectionBus").toString() == "usb";
            if (block.value("HintSystem").toBool() && !removable)
                continue;
            QString name = block.value("IdLabel").toString();
            if (name.isEmpty())
                name = block.value("HintName").toString();
            if (name.isEmpty())
                name = (drive.value("Vendor").toString() + " " + drive.value("Model").toString()).trimmed();
            const qint64 size = block.value("Size").toLongLong();
            const QString sizeText = QLocale().formattedDataSize(size, 1, QLocale::DataSizeSIFormat);
            if (name.isEmpty())
                name = sizeText + " Volume";
            const QStringList points = mountPoints(it->value(kFilesystem).value("MountPoints"));
            out.append(QVariantMap{{"path", it.key()},
                                   {"name", name},
                                   {"size", sizeText},
                                   {"device", bytes(block.value("Device"))},
                                   {"mounted", !points.isEmpty()},
                                   {"mountPoint", points.value(0)},
                                   {"removable", removable}});
            now.insert(it.key());
            // Just plugged in: mounted, and said.
            if (loaded_ && autoMount && removable && !seen_.contains(it.key()) && points.isEmpty() &&
                block.value("HintAuto").toBool())
                mount(it.key());
        }
        seen_ = now;
        loaded_ = true;
        QCollator order;
        std::sort(out.begin(), out.end(), [&](const QVariant& a, const QVariant& b) {
            return order.compare(a.toMap().value("name").toString(), b.toMap().value("name").toString()) < 0;
        });
        disks_ = out;
        emit changed();
    });
}

QVariantList Disks::ejectable() const {
    QVariantList out;
    for (const QVariant& d : disks_)
        if (d.toMap().value("removable").toBool() && d.toMap().value("mounted").toBool())
            out.append(d);
    return out;
}

QString Disks::drive(const QString& path) const {
    QDBusMessage m = QDBusMessage::createMethodCall(kUDisks, path, "org.freedesktop.DBus.Properties", "Get");
    m << kBlock << QStringLiteral("Drive");
    QDBusMessage r = bus().call(m, QDBus::Block, 2000);
    return r.arguments().isEmpty() ? QString()
                                   : r.arguments().first().value<QDBusVariant>().variant().value<QDBusObjectPath>().path();
}

void Disks::mount(const QString& path) {
    QDBusMessage m = QDBusMessage::createMethodCall(kUDisks, path, kFilesystem, "Mount");
    m << QVariantMap();
    m.setInteractiveAuthorizationAllowed(true);
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m, 5 * 60 * 1000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, path] {
        w->deleteLater();
        QDBusPendingReply<QString> r = *w;
        if (r.isError()) {
            emit failed(r.error().message());
            return;
        }
        for (const QVariant& d : disks_)
            if (d.toMap().value("path") == path)
                mounted(path, d.toMap().value("name").toString(), r.value());
    });
}

// Told once it's there, with a way to open it.
void Disks::mounted(const QString& path, const QString& name, const QString& where) {
    Q_UNUSED(path);
    if (QGuiApplication::desktopFileName() != "atrium-shell")
        return;
    QDBusMessage n = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                    "org.freedesktop.Notifications", "Notify");
    n << QString("Disks") << uint(0) << QString("drive-removable-media") << name
      << QString("Ready to use.") << QStringList{"open", "Open"} << QVariantMap() << -1;
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(n), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, where] {
        w->deleteLater();
        QDBusPendingReply<uint> r = *w;
        if (!r.isError())
            notices_.insert(r.value(), where);
    });
}

void Disks::actionInvoked(uint id, const QString& key) {
    if (key == "open" && notices_.contains(id))
        QProcess::startDetached("xdg-open", {notices_.take(id)});
}

void Disks::open(const QString& path) {
    for (const QVariant& d : disks_)
        if (d.toMap().value("path") == path && d.toMap().value("mounted").toBool())
            QProcess::startDetached("xdg-open", {d.toMap().value("mountPoint").toString()});
}

void Disks::unmount(const QString& path) {
    QDBusMessage m = QDBusMessage::createMethodCall(kUDisks, path, kFilesystem, "Unmount");
    m << QVariantMap();
    m.setInteractiveAuthorizationAllowed(true);
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m, 5 * 60 * 1000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<> r = *w;
        if (r.isError())
            emit failed(r.error().message());
    });
}

void Disks::eject(const QString& path) {
    const QString driveObject = drive(path);
    QDBusMessage m = QDBusMessage::createMethodCall(kUDisks, path, kFilesystem, "Unmount");
    m << QVariantMap();
    m.setInteractiveAuthorizationAllowed(true);
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m, 5 * 60 * 1000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, driveObject] {
        w->deleteLater();
        QDBusPendingReply<> r = *w;
        // Already unmounted is fine; anything else (busy) stops here.
        if (r.isError() && !r.error().name().endsWith("NotMounted")) {
            emit failed(r.error().message());
            return;
        }
        if (driveObject.isEmpty() || driveObject == "/")
            return;
        QDBusMessage off = QDBusMessage::createMethodCall(kUDisks, driveObject, kDrive, "PowerOff");
        off << QVariantMap();
        off.setInteractiveAuthorizationAllowed(true);
        bus().asyncCall(off);
    });
}

} // namespace atrium
