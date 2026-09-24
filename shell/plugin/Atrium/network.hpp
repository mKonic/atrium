#pragma once
// Networks through NetworkManager: `Network.wifiEnabled`, `Network.networks`
// (Wi-Fi networks by name, the connected one first, then by signal),
// `Network.current`, the wired devices, and whether the internet is really
// there (`limited` behind a captive portal or without a route out).

#include <QDBusMessage>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariant>

namespace atrium {

class Network;

class WifiNetwork : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(double signalStrength READ signalStrength NOTIFY changed)  // 0..1
    Q_PROPERTY(int bars READ bars NOTIFY changed)                          // 1..3
    Q_PROPERTY(int security READ security NOTIFY changed)                  // 0 open, 1 personal, 2 enterprise
    Q_PROPERTY(bool known READ known NOTIFY changed)                      // saved
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool stateChanging READ stateChanging NOTIFY changed)

public:
    WifiNetwork(Network* network, const QString& ssid);

    QString name() const { return ssid_; }
    double signalStrength() const { return strength_ / 100.0; }
    int bars() const { return strength_ > 66 ? 3 : strength_ > 33 ? 2 : 1; }
    int security() const { return security_; }
    bool known() const { return known_; }
    bool connected() const { return connected_; }
    bool stateChanging() const { return changing_; }

    Q_INVOKABLE void connect();
    Q_INVOKABLE void connectWithPsk(const QString& psk);
    Q_INVOKABLE void disconnect();
    Q_INVOKABLE void forget();

    void set(int strength, int security, const QString& ap, bool known, bool connected, bool changing);
    QString accessPoint() const { return ap_; }
    void failed(const QString& reason) { emit connectionFailed(reason); }

signals:
    void changed();
    void connectedChanged();
    // The last try to join didn't work (a wrong password, mostly).
    void connectionFailed(const QString& reason);

private:
    Network* network_;
    QString ssid_, ap_;
    int strength_ = 0, security_ = 0;
    bool known_ = false, connected_ = false, changing_ = false;
};

class WiredDevice : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(QString address READ address NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)

public:
    WiredDevice(const QString& path, QObject* parent) : QObject(parent), path_(path) {}

    QString path() const { return path_; }
    QString name() const { return name_; }
    QString address() const { return address_; }
    bool connected() const { return state_ == 100; }
    void set(const QString& name, const QString& address, uint state) {
        name_ = name;
        address_ = address;
        state_ = state;
        emit changed();
    }

signals:
    void changed();

private:
    QString path_, name_, address_;
    uint state_ = 0;
};

class Network : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)       // NetworkManager is running
    Q_PROPERTY(bool hasWifi READ hasWifi NOTIFY changed)
    Q_PROPERTY(bool wifiEnabled READ wifiEnabled WRITE setWifiEnabled NOTIFY changed)
    Q_PROPERTY(QList<QObject*> networks READ networks NOTIFY networksChanged)
    Q_PROPERTY(atrium::WifiNetwork* current READ current NOTIFY networksChanged)
    Q_PROPERTY(QList<QObject*> wired READ wired NOTIFY changed)
    Q_PROPERTY(atrium::WiredDevice* wiredConnection READ wiredConnection NOTIFY changed)
    Q_PROPERTY(bool limited READ limited NOTIFY changed)
    // While true (a Wi-Fi list is open), networks are looked for every so often.
    Q_PROPERTY(bool scanning READ scanning WRITE setScanning NOTIFY scanningChanged)

public:
    static Network* instance();

    bool available() const { return available_; }
    bool hasWifi() const { return !wifi_.isEmpty(); }
    bool wifiEnabled() const { return wifiEnabled_; }
    void setWifiEnabled(bool on);
    QList<QObject*> networks() const;
    WifiNetwork* current() const;
    QList<QObject*> wired() const;
    WiredDevice* wiredConnection() const;
    bool limited() const { return connectivity_ == 2 || connectivity_ == 3; }
    bool scanning() const { return scan_.isActive(); }
    void setScanning(bool on);

    // For WifiNetwork.
    void activate(WifiNetwork* n, const QString& psk);
    void deactivate(WifiNetwork* n);
    void forget(WifiNetwork* n);

signals:
    void changed();
    void networksChanged();
    void scanningChanged();

private slots:
    void propertiesChanged(const QDBusMessage& message);
    void deviceStateChanged(const QDBusMessage& message);
    void reloadSoon();

private:
    Network();
    void reload();
    void loadDevices(const QStringList& paths);
    void loadAccessPoints();
    void loadSaved();
    void rebuild();
    void requestScan();

    struct AccessPoint {
        QString ssid;
        int strength = 0;
        int security = 0;
    };

    bool available_ = false;
    bool wifiEnabled_ = false;
    uint connectivity_ = 0;
    QString wifi_;             // the Wi-Fi device's path
    QString activeAp_;         // its access point in use
    uint wifiState_ = 0;       // its NMDeviceState
    QString joining_;          // the SSID being joined, for failure reports
    QHash<QString, AccessPoint> aps_;
    QHash<QString, QStringList> saved_;  // SSID → saved connection paths
    QList<WifiNetwork*> networks_;       // kept by SSID, so QML's references stay
    QList<WiredDevice*> wired_;
    QTimer scan_;
    QTimer reload_;
};

} // namespace atrium
