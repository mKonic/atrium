#include "bluetooth.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

namespace atrium {

namespace {

const QString kBluez = QStringLiteral("org.bluez");
const QString kAdapter = QStringLiteral("org.bluez.Adapter1");
const QString kDevice = QStringLiteral("org.bluez.Device1");
const QString kBattery = QStringLiteral("org.bluez.Battery1");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

QDBusConnection bus() {
    return QDBusConnection::systemBus();
}

QVariant plain(const QVariant& v) {
    return v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

QMap<QString, QVariantMap> interfaces_of(const QDBusArgument& arg) {
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

} // namespace

// --- BluetoothDevice ---------------------------------------------------------

BluetoothDevice::BluetoothDevice(const QString& path, QObject* parent) : QObject(parent), path_(path) {
    bus().connect(kBluez, path, kProps, "PropertiesChanged", this,
                  SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
}

void BluetoothDevice::apply(const QString& interface, const QVariantMap& props) {
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QVariant v = plain(it.value());
        if (interface == kBattery) {
            if (it.key() == "Percentage")
                battery_ = v.toInt();
            continue;
        }
        const QString& k = it.key();
        if (k == "Adapter") adapter_ = v.value<QDBusObjectPath>().path();
        else if (k == "Name") name_ = v.toString();
        else if (k == "Alias") alias_ = v.toString();
        else if (k == "Address") address_ = v.toString();
        else if (k == "Icon") icon_ = v.toString();
        else if (k == "Paired") paired_ = v.toBool();
        else if (k == "Trusted") trusted_ = v.toBool();
        else if (k == "Connected") {
            connected_ = v.toBool();
            connecting_ = disconnecting_ = false;
        }
    }
    emit changed();
}

void BluetoothDevice::dropBattery() {
    battery_ = -1;
    emit changed();
}

void BluetoothDevice::propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList&) {
    if (interface == kDevice || interface == kBattery)
        apply(interface, changed);
}

int BluetoothDevice::state() const {
    if (connecting_)
        return BluetoothDeviceState::Connecting;
    if (disconnecting_)
        return BluetoothDeviceState::Disconnecting;
    return connected_ ? BluetoothDeviceState::Connected : BluetoothDeviceState::Disconnected;
}

bool BluetoothDevice::named() const {
    const QString n = name();
    return !n.isEmpty() && n != QString(address_).replace(':', '-') && n != address_;
}

void BluetoothDevice::call(const QString& method, std::function<void(bool)> done) {
    QDBusMessage m = QDBusMessage::createMethodCall(kBluez, path_, kDevice, method);
    // Pairing and connecting can take a while (the other side decides).
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m, 60'000), this);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [w, done] {
        w->deleteLater();
        if (done)
            done(!w->isError());
    });
}

void BluetoothDevice::connect() {
    connecting_ = true;
    emit changed();
    call("Connect", [this](bool) {
        connecting_ = false;
        emit changed();
    });
}

void BluetoothDevice::disconnect() {
    disconnecting_ = true;
    emit changed();
    call("Disconnect", [this](bool) {
        disconnecting_ = false;
        emit changed();
    });
}

void BluetoothDevice::pair() {
    pairing_ = true;
    emit changed();
    call("Pair", [this](bool ok) {
        pairing_ = false;
        emit changed();
        if (!ok)
            return;
        // Trusted, so it may reconnect by itself; then in use.
        QDBusMessage set = QDBusMessage::createMethodCall(kBluez, path_, kProps, "Set");
        set << kDevice << QStringLiteral("Trusted") << QVariant::fromValue(QDBusVariant(true));
        bus().asyncCall(set);
        connect();
    });
}

void BluetoothDevice::cancelPair() {
    call("CancelPairing");
}

void BluetoothDevice::forget() {
    QDBusMessage m = QDBusMessage::createMethodCall(kBluez, adapter_, kAdapter, "RemoveDevice");
    m << QVariant::fromValue(QDBusObjectPath(path_));
    bus().asyncCall(m);
}

// --- BluetoothAdapter --------------------------------------------------------

BluetoothAdapter::BluetoothAdapter(const QString& path, QObject* parent) : QObject(parent), path_(path) {
    bus().connect(kBluez, path, kProps, "PropertiesChanged", this,
                  SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
}

void BluetoothAdapter::apply(const QVariantMap& props) {
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QVariant v = plain(it.value());
        if (it.key() == "Name") name_ = v.toString();
        else if (it.key() == "Alias") alias_ = v.toString();
        else if (it.key() == "Powered") powered_ = v.toBool();
        else if (it.key() == "Discovering") discovering_ = v.toBool();
    }
    emit changed();
    // Turned on while something wants to look: look.
    if (powered_ && wantDiscovery_ && !discovering_)
        setDiscovering(true);
}

void BluetoothAdapter::propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList&) {
    if (interface == kAdapter)
        apply(changed);
}

void BluetoothAdapter::setBluez(const QString& name, const QVariant& value) {
    QDBusMessage m = QDBusMessage::createMethodCall(kBluez, path_, kProps, "Set");
    m << kAdapter << name << QVariant::fromValue(QDBusVariant(value));
    bus().asyncCall(m);
}

void BluetoothAdapter::setEnabled(bool on) {
    if (on == powered_)
        return;
    setBluez("Powered", on);
}

void BluetoothAdapter::setDiscovering(bool on) {
    wantDiscovery_ = on;
    if (on == discovering_ || (on && !powered_))
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kBluez, path_, kAdapter, on ? "StartDiscovery" : "StopDiscovery");
    bus().asyncCall(m);
}

void BluetoothAdapter::add(BluetoothDevice* d) {
    if (devices_.contains(d))
        return;
    d->setParent(this);
    devices_.append(d);
    QObject::connect(d, &BluetoothDevice::changed, this, &BluetoothAdapter::devicesChanged);
    emit devicesChanged();
}

void BluetoothAdapter::remove(BluetoothDevice* d) {
    if (devices_.removeOne(d))
        emit devicesChanged();
}

QList<QObject*> BluetoothAdapter::devices() const {
    return {devices_.begin(), devices_.end()};
}

QList<QObject*> BluetoothAdapter::mine() const {
    QList<QObject*> out;
    for (BluetoothDevice* d : devices_)
        if (d->paired() || d->connected())
            out.append(d);
    return out;
}

QList<QObject*> BluetoothAdapter::nearby() const {
    QList<QObject*> out;
    for (BluetoothDevice* d : devices_)
        if (!d->paired() && !d->connected() && d->named())
            out.append(d);
    return out;
}

QList<QObject*> BluetoothAdapter::connectedDevices() const {
    QList<QObject*> out;
    for (BluetoothDevice* d : devices_)
        if (d->connected())
            out.append(d);
    return out;
}

QString BluetoothAdapter::connectedNames() const {
    QStringList names;
    for (BluetoothDevice* d : devices_)
        if (d->connected())
            names.append(d->name());
    return names.join(", ");
}

// --- Bluetooth ---------------------------------------------------------------

Bluetooth* Bluetooth::instance() {
    static auto* self = new Bluetooth;
    return self;
}

Bluetooth::Bluetooth() {
    auto* watcher = new QDBusServiceWatcher(kBluez, bus(),
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &Bluetooth::serviceUp);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, &Bluetooth::serviceDown);
    bus().connect(kBluez, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this,
                  SLOT(interfacesAdded(QDBusMessage)));
    bus().connect(kBluez, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", this,
                  SLOT(interfacesRemoved(QDBusObjectPath, QStringList)));
    load();
}

void Bluetooth::serviceUp() {
    load();
}

void Bluetooth::serviceDown() {
    for (BluetoothAdapter* a : adapters_)
        a->deleteLater();
    adapters_.clear();
    for (BluetoothDevice* d : orphans_)
        d->deleteLater();
    orphans_.clear();
    emit adapterChanged();
}

void Bluetooth::load() {
    QDBusMessage m = QDBusMessage::createMethodCall(kBluez, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return;
        const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
        arg.beginMap();
        while (!arg.atEnd()) {
            QDBusObjectPath path;
            arg.beginMapEntry();
            arg >> path;
            const QMap<QString, QVariantMap> interfaces = interfaces_of(arg);
            arg.endMapEntry();
            added(path.path(), interfaces);
        }
        arg.endMap();
    });
}

void Bluetooth::interfacesAdded(const QDBusMessage& message) {
    if (message.arguments().size() < 2)
        return;
    const QString path = message.arguments().at(0).value<QDBusObjectPath>().path();
    added(path, interfaces_of(message.arguments().at(1).value<QDBusArgument>()));
}

void Bluetooth::added(const QString& path, const QMap<QString, QVariantMap>& interfaces) {
    if (interfaces.contains(kAdapter)) {
        BluetoothAdapter* a = adapterAt(path);
        if (!a) {
            a = new BluetoothAdapter(path, this);
            adapters_.append(a);
            a->apply(interfaces.value(kAdapter));
            // Devices that came before their adapter.
            for (qsizetype i = orphans_.size() - 1; i >= 0; --i)
                if (orphans_[i]->adapter() == path)
                    a->add(orphans_.takeAt(i));
            emit adapterChanged();
        } else {
            a->apply(interfaces.value(kAdapter));
        }
    }
    if (interfaces.contains(kDevice) || interfaces.contains(kBattery)) {
        BluetoothDevice* d = deviceAt(path);
        const bool fresh = !d;
        if (fresh)
            d = new BluetoothDevice(path, this);
        if (interfaces.contains(kDevice))
            d->apply(kDevice, interfaces.value(kDevice));
        if (interfaces.contains(kBattery))
            d->apply(kBattery, interfaces.value(kBattery));
        if (fresh) {
            if (BluetoothAdapter* a = adapterAt(d->adapter()))
                a->add(d);
            else
                orphans_.append(d);
        }
    }
}

void Bluetooth::interfacesRemoved(const QDBusObjectPath& p, const QStringList& interfaces) {
    const QString path = p.path();
    if (interfaces.contains(kDevice)) {
        if (BluetoothDevice* d = deviceAt(path)) {
            if (BluetoothAdapter* a = adapterAt(d->adapter()))
                a->remove(d);
            orphans_.removeOne(d);
            d->deleteLater();
        }
    } else if (interfaces.contains(kBattery)) {
        if (BluetoothDevice* d = deviceAt(path))
            d->dropBattery();
    }
    if (interfaces.contains(kAdapter)) {
        if (BluetoothAdapter* a = adapterAt(path)) {
            adapters_.removeOne(a);
            a->deleteLater();
            emit adapterChanged();
        }
    }
}

BluetoothAdapter* Bluetooth::adapterAt(const QString& path) const {
    for (BluetoothAdapter* a : adapters_)
        if (a->path() == path)
            return a;
    return nullptr;
}

BluetoothDevice* Bluetooth::deviceAt(const QString& path) const {
    for (BluetoothAdapter* a : adapters_)
        for (QObject* o : a->devices())
            if (static_cast<BluetoothDevice*>(o)->path() == path)
                return static_cast<BluetoothDevice*>(o);
    for (BluetoothDevice* d : orphans_)
        if (d->path() == path)
            return d;
    return nullptr;
}

} // namespace atrium
