#pragma once
// VPNs through NetworkManager: every WireGuard or VPN connection it has, and
// WireGuard configurations imported from a file or a provider's download
// (Mullvad's zip: one tunnel, a server per file). An imported tunnel's
// server is changed in place, by country or city, as Mullvad's app does.
//
//   Vpn.tunnels, Vpn.current (connected, or the last used), Vpn.connected
//   Vpn.importFile(url) → imported(name, servers) or importFailed(error)
//   tunnel.countries: [{code, name, recent, header, cities: [{code, name}]}], the
//   recently used first
//   tunnel.defaultPlace: where turning it on goes ("se", "se/sto"; "" the
//   last used)

#include "vpn_core.hpp"

#include <QDBusMessage>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariant>

class QNetworkAccessManager;

namespace atrium {

class Vpn;

class VpnTunnel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // Servers to choose from: imported by atrium, more than one.
    Q_PROPERTY(bool hasServers READ hasServers NOTIFY changed)
    Q_PROPERTY(QString server READ server NOTIFY changed)          // "se-sto-wg-001"
    Q_PROPERTY(QString country READ country NOTIFY changed)        // "se"
    Q_PROPERTY(QString city READ city NOTIFY changed)              // "sto"
    Q_PROPERTY(QString location READ location NOTIFY changed)      // "Sweden, Stockholm"
    Q_PROPERTY(QVariantList countries READ countries NOTIFY changed)
    Q_PROPERTY(int serverCount READ serverCount NOTIFY changed)
    Q_PROPERTY(QString defaultPlace READ defaultPlace WRITE setDefaultPlace NOTIFY changed)
    // For choosing it: [{value, label}], "Last Used" then every country and city.
    Q_PROPERTY(QVariantList places READ places NOTIFY changed)
    // Why the last try didn't work, until the next.
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    VpnTunnel(Vpn* vpn, const QString& path);

    QString path() const { return path_; }
    QString uuid() const { return uuid_; }
    QString name() const { return name_; }
    bool connected() const { return state_ == 2; }
    bool busy() const { return state_ == 1 || state_ == 3 || switching_; }
    bool hasServers() const { return servers_.size() > 1; }
    QString server() const { return server_; }
    QString country() const;
    QString city() const;
    QString location() const;
    QVariantList countries() const;
    int serverCount() const { return int(servers_.size()); }
    QString defaultPlace() const { return default_; }
    void setDefaultPlace(const QString& place);
    QVariantList places() const;
    QString error() const { return error_; }

    // On: at the default place when one is set.
    Q_INVOKABLE void connect();
    Q_INVOKABLE void disconnect();
    Q_INVOKABLE void toggle() { connected() || busy() ? disconnect() : connect(); }
    // A server in that country, or city: one of them at random, as
    // Mullvad's app picks.
    Q_INVOKABLE void choose(const QString& country, const QString& city = {});
    Q_INVOKABLE void remove();

    struct Server {
        vpn::Server server;
        QString countryName, cityName;
    };

private:
    friend class Vpn;
    void setState(uint state, const QString& active, const QString& device);
    void fail(const QString& why);
    void start();  // on, where it is
    const Server* pick(const QString& place) const;  // a random server there

    Vpn* vpn_;
    QString path_, uuid_, name_, active_, device_;
    uint state_ = 0;  // NMActiveConnectionState: 1 activating, 2 activated, 3 deactivating
    bool switching_ = false;
    bool wanted_ = false;  // asked to connect; a drop back to nothing is a failure
    QString error_;
    // Imported by atrium: its servers and interface.
    std::vector<Server> servers_;
    QString server_;
    QString default_;
    std::vector<std::string> recent_;  // places chosen, the latest first
    vpn::Config config_;  // without its key: NetworkManager keeps that

signals:
    void changed();
};

class Vpn : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)  // NetworkManager is running
    Q_PROPERTY(QList<QObject*> tunnels READ tunnels NOTIFY changed)
    Q_PROPERTY(atrium::VpnTunnel* current READ current NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)

public:
    static Vpn* instance();

    bool available() const { return available_; }
    QList<QObject*> tunnels() const;
    VpnTunnel* current() const;
    bool connected() const;
    bool importing() const { return importing_ > 0; }

    // A WireGuard configuration, or an archive of them (Mullvad's zip).
    Q_INVOKABLE void importFile(const QUrl& file);
    // The current tunnel on or off.
    Q_INVOKABLE void toggle();

    // For VpnTunnel.
    void activate(VpnTunnel* t);
    void deactivate(VpnTunnel* t);
    void moveTo(VpnTunnel* t, const QString& server);
    void remove(VpnTunnel* t);
    void remember(VpnTunnel* t);  // its default and recent places

signals:
    void changed();
    void importingChanged();
    void imported(const QString& name, int servers);
    void importFailed(const QString& error);

private slots:
    void propertiesChanged(const QDBusMessage& message);
    void reloadSoon() { reload_.start(); }

private:
    Vpn();
    void reload();
    void loadActive();
    void add(const vpn::Tunnel& tunnel, const QString& replacing);
    QMap<QString, QVariantMap> settingsFor(const QString& uuid, const QString& name, const QString& interface, const vpn::Config& config,
                            const std::vector<vpn::Peer>& peers) const;
    void fetchNames();
    // vpn.json, again if another process (Settings) wrote it since.
    bool load();
    void save();
    void apply(VpnTunnel* t) const;  // what's stored of it
    void storedChanged();

    bool available_ = false;
    QList<VpnTunnel*> tunnels_;  // kept by path, so QML's references stay
    QString last_;               // the uuid last connected
    int importing_ = 0;
    QTimer reload_;
    QNetworkAccessManager* net_ = nullptr;
    // What atrium keeps of a tunnel it imported, by uuid (vpn.json).
    QJsonObject stored_;
    QDateTime storedTime_;
};

} // namespace atrium
