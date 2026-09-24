#pragma once
// Bluetooth through BlueZ: `Bluetooth.adapter` with its devices, sorted
// into the lists the shell shows (yours, nearby, connected). Setting
// `adapter.discovering` looks for devices until it is set back.

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QMap>

#include <functional>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QVariant>

namespace atrium {

class BluetoothDevice : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY changed)           // the alias, what to call it
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY changed)  // what the device calls itself
    Q_PROPERTY(QString address READ address CONSTANT)
    Q_PROPERTY(QString icon READ icon NOTIFY changed)           // BlueZ's icon name ("audio-headset")
    Q_PROPERTY(QString glyph READ glyph NOTIFY changed)         // Material Symbols name for it
    Q_PROPERTY(bool paired READ paired NOTIFY changed)
    Q_PROPERTY(bool trusted READ trusted NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool pairing READ pairing NOTIFY changed)
    Q_PROPERTY(int state READ state NOTIFY changed)             // BluetoothDeviceState
    Q_PROPERTY(bool batteryAvailable READ batteryAvailable NOTIFY changed)
    Q_PROPERTY(double battery READ battery NOTIFY changed)      // 0..1

public:
    BluetoothDevice(const QString& path, QObject* parent);

    QString path() const { return path_; }
    QString adapter() const { return adapter_; }
    QString name() const { return alias_.isEmpty() ? name_ : alias_; }
    QString deviceName() const { return name_; }
    QString address() const { return address_; }
    QString icon() const { return icon_; }
    QString glyph() const;
    bool paired() const { return paired_; }
    bool trusted() const { return trusted_; }
    bool connected() const { return connected_; }
    bool pairing() const { return pairing_; }
    int state() const;
    bool batteryAvailable() const { return battery_ >= 0; }
    double battery() const { return battery_ < 0 ? 0 : battery_ / 100.0; }
    // Has a name of its own, not just its address.
    bool named() const;

    Q_INVOKABLE void connect();
    Q_INVOKABLE void disconnect();
    // Pairs, trusts, then connects.
    Q_INVOKABLE void pair();
    Q_INVOKABLE void cancelPair();
    Q_INVOKABLE void forget();

    void apply(const QString& interface, const QVariantMap& props);
    void dropBattery();

signals:
    void changed();
    // Paired, connected or named changed: which of the adapter's lists it is in.
    void placeChanged();

private slots:
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    void call(const QString& method, std::function<void(bool)> done = {});

    QString path_, adapter_, name_, alias_, address_, icon_;
    bool paired_ = false, trusted_ = false, connected_ = false, pairing_ = false;
    bool connecting_ = false, disconnecting_ = false;
    int battery_ = -1;
};

class BluetoothAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool discovering READ discovering WRITE setDiscovering NOTIFY changed)
    Q_PROPERTY(QList<QObject*> devices READ devices NOTIFY devicesChanged)
    // Paired or connected: the user's devices.
    Q_PROPERTY(QList<QObject*> mine READ mine NOTIFY devicesChanged)
    // Found nearby, not paired, with a name worth showing.
    Q_PROPERTY(QList<QObject*> nearby READ nearby NOTIFY devicesChanged)
    Q_PROPERTY(QList<QObject*> connectedDevices READ connectedDevices NOTIFY devicesChanged)
    Q_PROPERTY(QString connectedNames READ connectedNames NOTIFY devicesChanged)  // "AirPods, Mouse"

public:
    BluetoothAdapter(const QString& path, QObject* parent);

    QString path() const { return path_; }
    QString name() const { return alias_.isEmpty() ? name_ : alias_; }
    bool enabled() const { return powered_; }
    void setEnabled(bool on);
    bool discovering() const { return discovering_; }
    void setDiscovering(bool on);
    QList<QObject*> devices() const;
    QList<QObject*> mine() const;
    QList<QObject*> nearby() const;
    QList<QObject*> connectedDevices() const;
    QString connectedNames() const;

    void apply(const QVariantMap& props);
    void add(BluetoothDevice* d);
    void remove(BluetoothDevice* d);

signals:
    void changed();
    void devicesChanged();

private slots:
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    void setBluez(const QString& name, const QVariant& value);

    QString path_, name_, alias_;
    bool powered_ = false, discovering_ = false, wantDiscovery_ = false;
    QList<BluetoothDevice*> devices_;
};

class BluetoothDeviceState : public QObject {
    Q_OBJECT

public:
    enum Enum { Disconnected, Connecting, Connected, Disconnecting };
    Q_ENUM(Enum)
};

class Bluetooth : public QObject {
    Q_OBJECT
    Q_PROPERTY(atrium::BluetoothAdapter* adapter READ adapter NOTIFY adapterChanged)

public:
    static Bluetooth* instance();

    BluetoothAdapter* adapter() const { return adapters_.isEmpty() ? nullptr : adapters_.first(); }

signals:
    void adapterChanged();

private slots:
    void interfacesAdded(const QDBusMessage& message);
    void interfacesRemoved(const QDBusObjectPath& path, const QStringList& interfaces);
    void serviceUp();
    void serviceDown();

private:
    Bluetooth();
    void load();
    void added(const QString& path, const QMap<QString, QVariantMap>& interfaces);
    BluetoothAdapter* adapterAt(const QString& path) const;
    BluetoothDevice* deviceAt(const QString& path) const;

    QList<BluetoothAdapter*> adapters_;
    QList<BluetoothDevice*> orphans_;  // devices whose adapter hasn't shown up yet
};

} // namespace atrium
