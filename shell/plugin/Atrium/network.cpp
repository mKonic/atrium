#include "network.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

#include <algorithm>
#include <functional>

namespace atrium {

namespace {

const QString kNM = QStringLiteral("org.freedesktop.NetworkManager");
const QString kPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kDevice = QStringLiteral("org.freedesktop.NetworkManager.Device");
const QString kWireless = QStringLiteral("org.freedesktop.NetworkManager.Device.Wireless");
const QString kAp = QStringLiteral("org.freedesktop.NetworkManager.AccessPoint");
const QString kSettings = QStringLiteral("org.freedesktop.NetworkManager.Settings");
const QString kConnection = QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

constexpr uint kWifiType = 2, kEthernetType = 1;
constexpr uint kActivated = 100, kFailed = 120;
constexpr uint kSecretsWrong[] = {7, 8, 9, 10, 11};  // no secrets, supplicant disconnect/config/failed/timeout

using Settings = QMap<QString, QVariantMap>;

QDBusConnection bus() {
    return QDBusConnection::systemBus();
}

QVariant plain(const QVariant& v) {
    return v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

QStringList paths(const QVariant& v) {
    QStringList out;
    const QVariant p = plain(v);
    if (p.canConvert<QList<QDBusObjectPath>>()) {
        for (const QDBusObjectPath& o : p.value<QList<QDBusObjectPath>>())
            out.append(o.path());
    } else if (p.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument arg = p.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QDBusObjectPath o;
            arg >> o;
            out.append(o.path());
        }
        arg.endArray();
    }
    return out;
}

QString path_of(const QVariant& v) {
    return plain(v).value<QDBusObjectPath>().path();
}

// Security from an access point's flags: 0 open, 1 personal, 2 enterprise.
int security_of(uint flags, uint wpa, uint rsn) {
    if ((wpa | rsn) & 0x200)
        return 2;  // 802.1X
    return (flags & 1) || wpa || rsn ? 1 : 0;
}

// Several async calls; `done` runs after the last one answers.
struct Batch {
    int pending = 0;
    std::function<void()> done;
    void finish() {
        if (--pending == 0 && done)
            done();
    }
};

} // namespace

// --- WifiNetwork -------------------------------------------------------------

WifiNetwork::WifiNetwork(Network* network, const QString& ssid) : QObject(network), network_(network), ssid_(ssid) {}

void WifiNetwork::set(int strength, int security, const QString& ap, bool known, bool connected, bool changing) {
    const bool was = connected_;
    strength_ = strength;
    security_ = security;
    ap_ = ap;
    known_ = known;
    connected_ = connected;
    changing_ = changing;
    emit changed();
    if (was != connected_)
        emit connectedChanged();
}

void WifiNetwork::connect() {
    network_->activate(this, {});
}

void WifiNetwork::connectWithPsk(const QString& psk) {
    network_->activate(this, psk);
}

void WifiNetwork::disconnect() {
    network_->deactivate(this);
}

void WifiNetwork::forget() {
    network_->forget(this);
}

// --- Network -----------------------------------------------------------------

Network* Network::instance() {
    static auto* self = new Network;
    return self;
}

Network::Network() {
    qDBusRegisterMetaType<Settings>();
    reload_.setSingleShot(true);
    reload_.setInterval(150);  // NetworkManager changes things in bursts
    QObject::connect(&reload_, &QTimer::timeout, this, &Network::reload);
    scan_.setInterval(15'000);
    QObject::connect(&scan_, &QTimer::timeout, this, &Network::requestScan);

    // Any object of NetworkManager's changing: read again, shortly.
    bus().connect(kNM, QString(), kProps, "PropertiesChanged", this, SLOT(propertiesChanged(QDBusMessage)));
    bus().connect(kNM, QString(), kDevice, "StateChanged", this, SLOT(deviceStateChanged(QDBusMessage)));
    bus().connect(kNM, "/org/freedesktop/NetworkManager/Settings", kSettings, "NewConnection", this, SLOT(reloadSoon()));
    bus().connect(kNM, "/org/freedesktop/NetworkManager/Settings", kSettings, "ConnectionRemoved", this, SLOT(reloadSoon()));
    auto* watcher = new QDBusServiceWatcher(kNM, bus(),
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &Network::reloadSoon);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        available_ = false;
        emit changed();
    });
    loadSaved();
    reload();
}

void Network::reloadSoon() {
    reload_.start();
}

void Network::propertiesChanged(const QDBusMessage& message) {
    // Saved connections' own changes (Updated) reload those too.
    if (message.path().startsWith("/org/freedesktop/NetworkManager/Settings/"))
        loadSaved();
    reload_.start();
}

void Network::deviceStateChanged(const QDBusMessage& message) {
    if (message.path() != wifi_ || message.arguments().size() < 3)
        return;
    const uint state = message.arguments().at(0).toUInt();
    const uint reason = message.arguments().at(2).toUInt();
    if (state == kFailed && !joining_.isEmpty()) {
        const bool wrong = std::find(std::begin(kSecretsWrong), std::end(kSecretsWrong), reason) != std::end(kSecretsWrong);
        for (WifiNetwork* n : networks_)
            if (n->name() == joining_)
                n->failed(wrong ? QStringLiteral("wrong password") : QStringLiteral("couldn't connect"));
        joining_.clear();
    } else if (state == kActivated) {
        joining_.clear();
    }
    reload_.start();
}

void Network::reload() {
    static int generation = 0;
    const int gen = ++generation;
    auto getAll = [this](const QString& path, const QString& iface, std::function<void(const QVariantMap&)> then,
                         std::shared_ptr<Batch> batch) {
        QDBusMessage m = QDBusMessage::createMethodCall(kNM, path, kProps, "GetAll");
        m << iface;
        ++batch->pending;
        auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
        QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [w, then, batch] {
            w->deleteLater();
            QDBusPendingReply<QVariantMap> r = *w;
            if (!r.isError())
                then(r.value());
            batch->finish();
        });
    };

    auto batch = std::make_shared<Batch>();
    auto aps = std::make_shared<QHash<QString, AccessPoint>>();
    auto wired = std::make_shared<QList<std::tuple<QString, QString, QString, uint>>>();
    auto wifi = std::make_shared<QString>();
    auto activeAp = std::make_shared<QString>();
    auto wifiState = std::make_shared<uint>(0);
    auto root = std::make_shared<QVariantMap>();

    batch->done = [this, gen, aps, wired, wifi, activeAp, wifiState, root] {
        if (gen != generation)
            return;  // a newer read is on its way
        available_ = !root->isEmpty();
        wifiEnabled_ = plain(root->value("WirelessEnabled")).toBool();
        connectivity_ = plain(root->value("Connectivity")).toUInt();
        wifi_ = *wifi;
        activeAp_ = *activeAp;
        wifiState_ = *wifiState;
        aps_ = *aps;
        // Wired devices, kept by path.
        QList<WiredDevice*> keep;
        for (const auto& [path, name, address, state] : *wired) {
            WiredDevice* d = nullptr;
            for (WiredDevice* w : wired_)
                if (w->path() == path)
                    d = w;
            if (!d)
                d = new WiredDevice(path, this);
            d->set(name, address, state);
            keep.append(d);
        }
        for (WiredDevice* w : wired_)
            if (!keep.contains(w))
                w->deleteLater();
        wired_ = keep;
        emit changed();
        rebuild();  // networksChanged: the glyph too
    };

    getAll(kPath, kNM, [this, root, batch, getAll, aps, wired, wifi, activeAp, wifiState](const QVariantMap& props) {
        *root = props;
        for (const QString& dev : paths(props.value("Devices"))) {
            getAll(dev, kDevice, [dev, batch, getAll, aps, wired, wifi, activeAp, wifiState](const QVariantMap& d) {
                const uint type = plain(d.value("DeviceType")).toUInt();
                const uint state = plain(d.value("State")).toUInt();
                if (type == kEthernetType) {
                    wired->append({dev, plain(d.value("Interface")).toString(), plain(d.value("HwAddress")).toString(), state});
                    return;
                }
                if (type != kWifiType || !wifi->isEmpty())
                    return;
                *wifi = dev;
                *wifiState = state;
                getAll(dev, kWireless, [batch, getAll, aps, activeAp](const QVariantMap& w) {
                    *activeAp = path_of(w.value("ActiveAccessPoint"));
                    for (const QString& ap : paths(w.value("AccessPoints"))) {
                        getAll(ap, kAp, [ap, aps](const QVariantMap& a) {
                            AccessPoint p;
                            p.ssid = QString::fromUtf8(plain(a.value("Ssid")).toByteArray());
                            p.strength = plain(a.value("Strength")).toInt();
                            p.security = security_of(plain(a.value("Flags")).toUInt(), plain(a.value("WpaFlags")).toUInt(),
                                                     plain(a.value("RsnFlags")).toUInt());
                            if (!p.ssid.isEmpty())  // hidden networks have no name to show
                                aps->insert(ap, p);
                        }, batch);
                    }
                }, batch);
            }, batch);
        }
    }, batch);
}

void Network::loadSaved() {
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, "/org/freedesktop/NetworkManager/Settings", kSettings, "ListConnections");
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<QList<QDBusObjectPath>> r = *w;
        if (r.isError())
            return;
        auto saved = std::make_shared<QHash<QString, QStringList>>();
        auto batch = std::make_shared<Batch>();
        batch->done = [this, saved] {
            saved_ = *saved;
            rebuild();
        };
        batch->pending = 1;  // held until every call is out
        for (const QDBusObjectPath& p : r.value()) {
            const QString path = p.path();
            ++batch->pending;
            QDBusMessage get = QDBusMessage::createMethodCall(kNM, path, kConnection, "GetSettings");
            auto* g = new QDBusPendingCallWatcher(bus().asyncCall(get), this);
            QObject::connect(g, &QDBusPendingCallWatcher::finished, this, [g, path, saved, batch] {
                g->deleteLater();
                QDBusPendingReply<Settings> s = *g;
                if (!s.isError()) {
                    const Settings& settings = s.value();
                    if (settings.value("connection").value("type").toString() == "802-11-wireless") {
                        const QString ssid = QString::fromUtf8(settings.value("802-11-wireless").value("ssid").toByteArray());
                        (*saved)[ssid].append(path);
                    }
                }
                batch->finish();
            });
        }
        batch->finish();
    });
}

void Network::rebuild() {
    // One entry per name: its strongest access point.
    struct Best {
        QString ap;
        int strength = -1, security = 0;
    };
    QHash<QString, Best> best;
    for (auto it = aps_.begin(); it != aps_.end(); ++it) {
        Best& b = best[it->ssid];
        if (it->strength > b.strength)
            b = {it.key(), it->strength, it->security};
    }
    const QString currentSsid = aps_.contains(activeAp_) ? aps_.value(activeAp_).ssid : QString();
    // Activating (40..90) or deactivating (110).
    const bool changing = (wifiState_ >= 40 && wifiState_ < kActivated) || wifiState_ == 110;

    QList<WifiNetwork*> list;
    for (auto it = best.begin(); it != best.end(); ++it) {
        WifiNetwork* n = nullptr;
        for (WifiNetwork* old : networks_)
            if (old->name() == it.key())
                n = old;
        if (!n)
            n = new WifiNetwork(this, it.key());
        const bool isCurrent = it.key() == currentSsid;
        n->set(it->strength, it->security, it->ap, saved_.contains(it.key()), isCurrent && wifiState_ == kActivated,
               (isCurrent || it.key() == joining_) && changing);
        list.append(n);
    }
    for (WifiNetwork* old : networks_)
        if (!list.contains(old))
            old->deleteLater();
    std::sort(list.begin(), list.end(), [](WifiNetwork* a, WifiNetwork* b) {
        if (a->connected() != b->connected())
            return a->connected();
        return a->signalStrength() > b->signalStrength();
    });
    networks_ = list;
    emit networksChanged();
}

QList<QObject*> Network::networks() const {
    return {networks_.begin(), networks_.end()};
}

WifiNetwork* Network::current() const {
    for (WifiNetwork* n : networks_)
        if (n->connected())
            return n;
    return nullptr;
}

QString Network::glyph() const {
    if (wiredConnection())
        return QStringLiteral("lan");
    if (WifiNetwork* n = current()) {
        if (limited())
            return QStringLiteral("wifi_find");
        return n->bars() == 3 ? QStringLiteral("wifi") : n->bars() == 2 ? QStringLiteral("wifi_2_bar") : QStringLiteral("wifi_1_bar");
    }
    return hasWifi() && wifiEnabled_ ? QStringLiteral("wifi_off") : QStringLiteral("signal_disconnected");
}

QList<QObject*> Network::saved() const {
    QList<QObject*> out;
    for (WifiNetwork* n : networks_)
        if (n->known() || n->connected())
            out.append(n);
    return out;
}

QList<QObject*> Network::unsaved() const {
    QList<QObject*> out;
    for (WifiNetwork* n : networks_)
        if (!n->known() && !n->connected())
            out.append(n);
    return out;
}

QList<QObject*> Network::wired() const {
    return {wired_.begin(), wired_.end()};
}

WiredDevice* Network::wiredConnection() const {
    for (WiredDevice* d : wired_)
        if (d->connected())
            return d;
    return nullptr;
}

void Network::setWifiEnabled(bool on) {
    if (on == wifiEnabled_)
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, kPath, kProps, "Set");
    m << kNM << QStringLiteral("WirelessEnabled") << QVariant::fromValue(QDBusVariant(on));
    bus().asyncCall(m);
}

void Network::setScanning(bool on) {
    if (on == scan_.isActive())
        return;
    if (on) {
        requestScan();
        scan_.start();
    } else {
        scan_.stop();
    }
    emit scanningChanged();
}

void Network::requestScan() {
    if (wifi_.isEmpty() || !wifiEnabled_)
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, wifi_, kWireless, "RequestScan");
    m << QVariantMap();
    bus().asyncCall(m);
}

void Network::activate(WifiNetwork* n, const QString& psk) {
    if (wifi_.isEmpty())
        return;
    joining_ = n->name();
    const QDBusObjectPath device(wifi_);
    const QDBusObjectPath ap(n->accessPoint().isEmpty() ? QStringLiteral("/") : n->accessPoint());
    auto report = [this, n](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (w->isError()) {
            joining_.clear();
            n->failed(w->error().message());
        }
    };
    const QStringList saved = saved_.value(n->name());
    if (!saved.isEmpty() && psk.isEmpty()) {
        QDBusMessage m = QDBusMessage::createMethodCall(kNM, kPath, kNM, "ActivateConnection");
        m << QVariant::fromValue(QDBusObjectPath(saved.first())) << QVariant::fromValue(device) << QVariant::fromValue(ap);
        auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
        QObject::connect(w, &QDBusPendingCallWatcher::finished, this, report);
        return;
    }
    // A new password replaces what was saved.
    for (const QString& path : saved)
        bus().asyncCall(QDBusMessage::createMethodCall(kNM, path, kConnection, "Delete"));
    Settings s;
    s["connection"] = {{"type", "802-11-wireless"}, {"id", n->name()}};
    s["802-11-wireless"] = {{"ssid", n->name().toUtf8()}};
    if (!psk.isEmpty())
        s["802-11-wireless-security"] = {{"key-mgmt", "wpa-psk"}, {"psk", psk}};
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, kPath, kNM, "AddAndActivateConnection");
    m << QVariant::fromValue(s) << QVariant::fromValue(device) << QVariant::fromValue(ap);
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, report);
}

void Network::deactivate(WifiNetwork* n) {
    if (wifi_.isEmpty() || !n->connected())
        return;
    bus().asyncCall(QDBusMessage::createMethodCall(kNM, wifi_, kDevice, "Disconnect"));
}

void Network::forget(WifiNetwork* n) {
    for (const QString& path : saved_.value(n->name()))
        bus().asyncCall(QDBusMessage::createMethodCall(kNM, path, kConnection, "Delete"));
}

} // namespace atrium
