#include "vpn.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <archive.h>
#include <archive_entry.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <map>

namespace atrium {

namespace {

const QString kNM = QStringLiteral("org.freedesktop.NetworkManager");
const QString kPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString kSettingsPath = QStringLiteral("/org/freedesktop/NetworkManager/Settings");
const QString kSettings = QStringLiteral("org.freedesktop.NetworkManager.Settings");
const QString kConnection = QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection");
const QString kActive = QStringLiteral("org.freedesktop.NetworkManager.Connection.Active");
const QString kDevice = QStringLiteral("org.freedesktop.NetworkManager.Device");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
// Mullvad's list of its servers, for the names of their countries and cities.
const QString kMullvadRelays = QStringLiteral("https://api.mullvad.net/www/relays/wireguard/");

using Settings = QMap<QString, QVariantMap>;
using MapList = QList<QVariantMap>;

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

QString file() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/atrium/vpn.json";
}

QString country_name(const QString& code) {
    const QLocale::Territory t = QLocale::codeToTerritory(code.toUpper());
    return t == QLocale::AnyTerritory ? code.toUpper() : QLocale::territoryToString(t);
}

QJsonObject to_json(const vpn::Config& c) {
    QJsonArray addresses;
    for (const vpn::Address& a : c.addresses)
        addresses.append(QJsonObject{{"ip", QString::fromStdString(a.ip)}, {"prefix", a.prefix}, {"v6", a.v6}});
    auto strings = [](const std::vector<std::string>& v) {
        QJsonArray out;
        for (const std::string& s : v)
            out.append(QString::fromStdString(s));
        return out;
    };
    return {{"addresses", addresses}, {"dns4", strings(c.dns4)}, {"dns6", strings(c.dns6)},
            {"dnsSearch", strings(c.dns_search)}, {"mtu", int(c.mtu)}, {"listenPort", int(c.listen_port)},
            {"fwmark", double(c.fwmark)}, {"table", double(c.table)}};
}

vpn::Config config_from(const QJsonObject& o) {
    vpn::Config c;
    for (const QJsonValue& a : o["addresses"].toArray())
        c.addresses.push_back({a["ip"].toString().toStdString(), a["prefix"].toInt(), a["v6"].toBool()});
    auto strings = [](const QJsonValue& v) {
        std::vector<std::string> out;
        for (const QJsonValue& s : v.toArray())
            out.push_back(s.toString().toStdString());
        return out;
    };
    c.dns4 = strings(o["dns4"]);
    c.dns6 = strings(o["dns6"]);
    c.dns_search = strings(o["dnsSearch"]);
    c.mtu = unsigned(o["mtu"].toInt());
    c.listen_port = unsigned(o["listenPort"].toInt());
    c.fwmark = unsigned(o["fwmark"].toDouble());
    c.table = (long long)o["table"].toDouble(vpn::kTableAuto);
    return c;
}

QJsonObject to_json(const VpnTunnel::Server& s) {
    QJsonArray ips;
    for (const std::string& ip : s.server.peer.allowed_ips)
        ips.append(QString::fromStdString(ip));
    QJsonObject o{{"id", QString::fromStdString(s.server.id)}, {"country", QString::fromStdString(s.server.country)},
                  {"city", QString::fromStdString(s.server.city)}, {"countryName", s.countryName},
                  {"cityName", s.cityName}, {"publicKey", QString::fromStdString(s.server.peer.public_key)},
                  {"endpoint", QString::fromStdString(s.server.peer.endpoint)}, {"allowedIps", ips}};
    if (s.server.peer.keepalive)
        o["keepalive"] = s.server.peer.keepalive;
    return o;
}

VpnTunnel::Server server_from(const QJsonObject& o) {
    VpnTunnel::Server s;
    s.server.id = o["id"].toString().toStdString();
    s.server.country = o["country"].toString().toStdString();
    s.server.city = o["city"].toString().toStdString();
    s.countryName = o["countryName"].toString();
    s.cityName = o["cityName"].toString();
    s.server.peer.public_key = o["publicKey"].toString().toStdString();
    s.server.peer.endpoint = o["endpoint"].toString().toStdString();
    for (const QJsonValue& ip : o["allowedIps"].toArray())
        s.server.peer.allowed_ips.push_back(ip.toString().toStdString());
    s.server.peer.keepalive = o["keepalive"].toInt();
    return s;
}

// The .conf files of an archive (Mullvad's zip) or the one file itself.
bool read_configs(const QString& path, std::vector<std::pair<std::string, std::string>>& out, QString& error) {
    if (path.endsWith(".conf")) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            error = f.errorString();
            return false;
        }
        out.emplace_back(QFileInfo(path).fileName().toStdString(), f.read(1 << 20).toStdString());
        return true;
    }
    archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 16384) != ARCHIVE_OK) {
        error = QString::fromUtf8(archive_error_string(a));
        archive_read_free(a);
        return false;
    }
    archive_entry* e = nullptr;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        const std::string name = archive_entry_pathname(e);
        if (archive_entry_filetype(e) != AE_IFREG || !name.ends_with(".conf") || archive_entry_size(e) > (1 << 20))
            continue;
        std::string text(size_t(archive_entry_size(e)), '\0');
        if (archive_read_data(a, text.data(), text.size()) == la_ssize_t(text.size()))
            out.emplace_back(name, std::move(text));
    }
    archive_read_free(a);
    if (out.empty())
        error = "No WireGuard configurations (.conf) in it.";
    return !out.empty();
}

} // namespace

// --- VpnTunnel -------------------------------------------------------------------

VpnTunnel::VpnTunnel(Vpn* vpn, const QString& path) : QObject(vpn), vpn_(vpn), path_(path) {}

QString VpnTunnel::country() const {
    for (const Server& s : servers_)
        if (QString::fromStdString(s.server.id) == server_)
            return QString::fromStdString(s.server.country);
    return {};
}

QString VpnTunnel::city() const {
    for (const Server& s : servers_)
        if (QString::fromStdString(s.server.id) == server_)
            return QString::fromStdString(s.server.city);
    return {};
}

QString VpnTunnel::location() const {
    for (const Server& s : servers_)
        if (QString::fromStdString(s.server.id) == server_)
            return s.countryName.isEmpty() ? server_ : s.countryName + ", " + s.cityName;
    return {};
}

QVariantList VpnTunnel::countries() const {
    // Country → city → how many servers, by name.
    struct City {
        QString name;
        int servers = 0;
    };
    struct Country {
        QString name;
        std::map<QString, City> cities;
    };
    std::map<QString, Country> all;
    for (const Server& s : servers_) {
        if (s.server.country.empty())
            continue;
        Country& c = all[QString::fromStdString(s.server.country)];
        c.name = s.countryName;
        City& city = c.cities[QString::fromStdString(s.server.city)];
        city.name = s.cityName;
        ++city.servers;
    }
    // By name, then the recently used first.
    auto ranked = [this](const auto& map, std::string_view prefix) {
        std::vector<std::string> codes;
        for (const auto& [code, v] : map)
            codes.push_back(code.toStdString());
        std::sort(codes.begin(), codes.end(), [&](const std::string& a, const std::string& b) {
            return QString::localeAwareCompare(map.at(QString::fromStdString(a)).name, map.at(QString::fromStdString(b)).name) < 0;
        });
        return vpn::by_recent(codes, recent_, prefix);
    };
    std::vector<std::string> recentCountries;
    for (const std::string& r : recent_)
        recentCountries.push_back(r.substr(0, r.find('/')));
    QVariantList out;
    for (const std::string& code : ranked(all, {})) {
        const Country& c = all.at(QString::fromStdString(code));
        QVariantList list;
        for (const std::string& cityCode : ranked(c.cities, code + "/")) {
            const City& city = c.cities.at(QString::fromStdString(cityCode));
            list.append(QVariantMap{{"code", QString::fromStdString(cityCode)}, {"name", city.name}, {"servers", city.servers}});
        }
        const bool recent = std::find(recentCountries.begin(), recentCountries.end(), code) != recentCountries.end();
        // A heading over the first recent one, and over the rest after them.
        QString header;
        if (recent && out.isEmpty())
            header = "Recent";
        else if (!recent && !out.isEmpty() && out.last().toMap()["recent"].toBool())
            header = "All Locations";
        out.append(QVariantMap{{"code", QString::fromStdString(code)}, {"name", c.name}, {"recent", recent},
                               {"header", header}, {"cities", list}});
    }
    return out;
}

QVariantList VpnTunnel::places() const {
    QVariantList out{QVariantMap{{"value", ""}, {"label", "Last Used"}}};
    for (const QVariant& v : countries()) {
        const QVariantMap c = v.toMap();
        const QVariantList cities = c["cities"].toList();
        out.append(QVariantMap{{"value", c["code"]}, {"label", c["name"]}});
        if (cities.size() > 1)
            for (const QVariant& city : cities)
                out.append(QVariantMap{{"value", c["code"].toString() + "/" + city.toMap()["code"].toString()},
                                       {"label", c["name"].toString() + ", " + city.toMap()["name"].toString()}});
    }
    return out;
}

void VpnTunnel::setDefaultPlace(const QString& place) {
    if (place == default_)
        return;
    default_ = place;
    vpn_->remember(this);
    emit changed();
}

const VpnTunnel::Server* VpnTunnel::pick(const QString& place) const {
    std::vector<const Server*> matching;
    for (const Server& s : servers_)
        if (vpn::within(place.toStdString(), s.server.country, s.server.city))
            matching.push_back(&s);
    return matching.empty() ? nullptr : matching[QRandomGenerator::global()->bounded(int(matching.size()))];
}

void VpnTunnel::connect() {
    // Somewhere else than the default place: there first.
    if (hasServers() && !default_.isEmpty() && !vpn::within(default_.toStdString(), country().toStdString(), city().toStdString())) {
        if (const Server* s = pick(default_)) {
            error_.clear();
            wanted_ = true;
            vpn_->moveTo(this, QString::fromStdString(s->server.id));
            return;
        }
    }
    start();
}

void VpnTunnel::start() {
    error_.clear();
    wanted_ = true;
    emit changed();
    vpn_->activate(this);
}

void VpnTunnel::disconnect() {
    wanted_ = false;
    vpn_->deactivate(this);
}

void VpnTunnel::choose(const QString& country, const QString& city) {
    const QString place = city.isEmpty() ? country : country + "/" + city;
    const Server* s = pick(place);
    if (!s)
        return;
    recent_ = vpn::used(recent_, place.toStdString());
    vpn_->remember(this);
    emit changed();
    // Already there: stay on the same server.
    if (vpn::within(place.toStdString(), this->country().toStdString(), this->city().toStdString())) {
        if (!connected() && !busy())
            start();
        return;
    }
    error_.clear();
    wanted_ = true;
    vpn_->moveTo(this, QString::fromStdString(s->server.id));
}

void VpnTunnel::remove() {
    vpn_->remove(this);
}

void VpnTunnel::setState(uint state, const QString& active, const QString& device) {
    // Asked for and gone again without getting there: it didn't work.
    if (wanted_ && state_ == 1 && (state == 0 || state == 4))
        fail("Couldn't connect");
    if (state == 2)
        wanted_ = false;
    state_ = state == 4 ? 0 : state;
    active_ = active;
    device_ = device;
    emit changed();
}

void VpnTunnel::fail(const QString& why) {
    wanted_ = false;
    switching_ = false;
    error_ = why;
    emit changed();
}

// --- Vpn -------------------------------------------------------------------------

Vpn* Vpn::instance() {
    static auto* self = new Vpn;
    return self;
}

Vpn::Vpn() {
    qDBusRegisterMetaType<Settings>();
    qDBusRegisterMetaType<MapList>();
    qDBusRegisterMetaType<QList<uint>>();
    qDBusRegisterMetaType<QList<QByteArray>>();
    load();
    // Its folder: the file itself is replaced on every save.
    QDir().mkpath(QFileInfo(file()).path());
    auto* watch = new QFileSystemWatcher({QFileInfo(file()).path()}, this);
    QObject::connect(watch, &QFileSystemWatcher::directoryChanged, this, &Vpn::storedChanged);
    reload_.setSingleShot(true);
    reload_.setInterval(150);  // NetworkManager changes things in bursts
    QObject::connect(&reload_, &QTimer::timeout, this, &Vpn::reload);
    bus().connect(kNM, QString(), kProps, "PropertiesChanged", this, SLOT(propertiesChanged(QDBusMessage)));
    bus().connect(kNM, kSettingsPath, kSettings, "NewConnection", this, SLOT(reloadSoon()));
    bus().connect(kNM, kSettingsPath, kSettings, "ConnectionRemoved", this, SLOT(reloadSoon()));
    auto* watcher = new QDBusServiceWatcher(kNM, bus(),
        QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration, this);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, &Vpn::reloadSoon);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        available_ = false;
        emit changed();
    });
    reload();
}

QList<QObject*> Vpn::tunnels() const {
    QList<QObject*> out;
    for (VpnTunnel* t : tunnels_)
        out.append(t);
    return out;
}

VpnTunnel* Vpn::current() const {
    for (VpnTunnel* t : tunnels_)
        if (t->connected() || t->busy())
            return t;
    for (VpnTunnel* t : tunnels_)
        if (t->uuid() == last_)
            return t;
    return tunnels_.isEmpty() ? nullptr : tunnels_.first();
}

bool Vpn::connected() const {
    return std::any_of(tunnels_.begin(), tunnels_.end(), [](VpnTunnel* t) { return t->connected(); });
}

void Vpn::toggle() {
    if (VpnTunnel* t = current())
        t->toggle();
}

void Vpn::propertiesChanged(const QDBusMessage& message) {
    // Connections and their activations; not every access point's signal.
    const QString iface = message.arguments().value(0).toString();
    if (iface == kActive || iface == kNM || iface == kConnection)
        reloadSoon();
}

void Vpn::reload() {
    load();
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, kSettingsPath, kSettings, "ListConnections");
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        QDBusPendingReply<QList<QDBusObjectPath>> reply = *w;
        if (reply.isError()) {
            available_ = false;
            emit changed();
            return;
        }
        available_ = true;
        const QList<QDBusObjectPath> all = reply.value();
        // Each one's settings: which are VPNs, and what they're called.
        auto pending = std::make_shared<int>(int(all.size()));
        auto found = std::make_shared<QList<std::tuple<QString, QString, QString>>>();  // path, uuid, name
        auto done = [this, found] {
            QList<VpnTunnel*> next;
            for (const auto& [path, uuid, name] : *found) {
                auto it = std::find_if(tunnels_.begin(), tunnels_.end(), [&](VpnTunnel* t) { return t->path() == path; });
                VpnTunnel* t = it != tunnels_.end() ? *it : new VpnTunnel(this, path);
                t->uuid_ = uuid;
                t->name_ = name;
                // What atrium knows of it: its servers.
                apply(t);
                next.append(t);
            }
            for (VpnTunnel* t : tunnels_)
                if (!next.contains(t))
                    t->deleteLater();
            std::sort(next.begin(), next.end(), [](VpnTunnel* a, VpnTunnel* b) { return QString::localeAwareCompare(a->name(), b->name()) < 0; });
            tunnels_ = next;
            for (VpnTunnel* t : tunnels_)
                emit t->changed();
            emit changed();
            loadActive();
        };
        if (all.isEmpty()) {
            done();
            return;
        }
        for (const QDBusObjectPath& p : all) {
            QDBusMessage get = QDBusMessage::createMethodCall(kNM, p.path(), kConnection, "GetSettings");
            auto* g = new QDBusPendingCallWatcher(bus().asyncCall(get), this);
            QObject::connect(g, &QDBusPendingCallWatcher::finished, this, [path = p.path(), pending, found, done](QDBusPendingCallWatcher* g) {
                g->deleteLater();
                QDBusPendingReply<Settings> s = *g;
                if (!s.isError()) {
                    const QVariantMap c = s.value().value("connection");
                    const QString type = c.value("type").toString();
                    if (type == "wireguard" || type == "vpn")
                        found->append({path, c.value("uuid").toString(), c.value("id").toString()});
                }
                if (--*pending == 0)
                    done();
            });
        }
    });
}

void Vpn::apply(VpnTunnel* t) const {
    const QJsonObject stored = stored_[t->uuid()].toObject();
    t->servers_.clear();
    for (const QJsonValue& s : stored["servers"].toArray())
        t->servers_.push_back(server_from(s.toObject()));
    t->server_ = stored["server"].toString();
    t->default_ = stored["default"].toString();
    t->recent_.clear();
    for (const QJsonValue& r : stored["recent"].toArray())
        t->recent_.push_back(r.toString().toStdString());
    t->config_ = config_from(stored["config"].toObject());
}

void Vpn::storedChanged() {
    // Written by another process (Settings, or the shell): its default,
    // recent places and server here too.
    if (!load())
        return;
    for (VpnTunnel* t : tunnels_) {
        apply(t);
        emit t->changed();
    }
    emit changed();
}

void Vpn::loadActive() {
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, kPath, kProps, "Get");
    m << kNM << QStringLiteral("ActiveConnections");
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        QDBusPendingReply<QVariant> reply = *w;
        const QStringList actives = reply.isError() ? QStringList() : paths(reply.value());
        // Connection path → (state, active path, device).
        auto states = std::make_shared<QHash<QString, std::tuple<uint, QString, QString>>>();
        auto pending = std::make_shared<int>(int(actives.size()));
        auto done = [this, states] {
            for (VpnTunnel* t : tunnels_) {
                const auto s = states->value(t->path(), {0u, QString(), QString()});
                if (std::get<0>(s) == 2 && t->uuid() != last_) {
                    load();
                    last_ = t->uuid();
                    save();
                }
                t->setState(std::get<0>(s), std::get<1>(s), std::get<2>(s));
            }
            emit changed();
        };
        if (actives.isEmpty()) {
            done();
            return;
        }
        for (const QString& a : actives) {
            QDBusMessage get = QDBusMessage::createMethodCall(kNM, a, kProps, "GetAll");
            get << kActive;
            auto* g = new QDBusPendingCallWatcher(bus().asyncCall(get), this);
            QObject::connect(g, &QDBusPendingCallWatcher::finished, this, [a, states, pending, done](QDBusPendingCallWatcher* g) {
                g->deleteLater();
                QDBusPendingReply<QVariantMap> r = *g;
                if (!r.isError()) {
                    const QVariantMap p = r.value();
                    const QString connection = plain(p.value("Connection")).value<QDBusObjectPath>().path();
                    const QStringList devices = paths(p.value("Devices"));
                    states->insert(connection, {plain(p.value("State")).toUInt(), a, devices.value(0)});
                }
                if (--*pending == 0)
                    done();
            });
        }
    });
}

void Vpn::activate(VpnTunnel* t) {
    // One VPN at a time: the one on now goes off.
    for (VpnTunnel* other : tunnels_)
        if (other != t && (other->connected() || other->busy()))
            deactivate(other);
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, kPath, kNM, "ActivateConnection");
    m << QVariant::fromValue(QDBusObjectPath(t->path())) << QVariant::fromValue(QDBusObjectPath("/"))
      << QVariant::fromValue(QDBusObjectPath("/"));
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QPointer<VpnTunnel> tunnel(t);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [tunnel](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (w->isError() && tunnel)
            tunnel->fail(w->error().message());
    });
}

void Vpn::deactivate(VpnTunnel* t) {
    if (t->active_.isEmpty())
        return;
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, kPath, kNM, "DeactivateConnection");
    m << QVariant::fromValue(QDBusObjectPath(t->active_));
    bus().asyncCall(m);
}

Settings Vpn::settingsFor(const QString& uuid, const QString& name, const QString& interface, const vpn::Config& c,
                             const std::vector<vpn::Peer>& peers) const {
    // As nm_conn_wireguard_import makes them, a profile of this user's own.
    QString user = qEnvironmentVariable("USER");
    if (const passwd* pw = getpwuid(getuid()))
        user = QString::fromLocal8Bit(pw->pw_name);
    Settings s;
    s["connection"] = {{"id", name}, {"uuid", uuid}, {"type", "wireguard"}, {"interface-name", interface},
                       {"autoconnect", false}, {"permissions", QStringList{"user:" + user}}};
    MapList peerList;
    for (const vpn::Peer& p : peers) {
        QVariantMap m{{"public-key", QString::fromStdString(p.public_key)}};
        if (!p.endpoint.empty())
            m["endpoint"] = QString::fromStdString(p.endpoint);
        QStringList ips;
        for (const std::string& ip : p.allowed_ips)
            ips.append(QString::fromStdString(ip));
        m["allowed-ips"] = ips;
        if (p.keepalive)
            m["persistent-keepalive"] = uint(p.keepalive);
        if (!p.preshared_key.empty()) {
            m["preshared-key"] = QString::fromStdString(p.preshared_key);
            m["preshared-key-flags"] = uint(0);
        }
        peerList.append(m);
    }
    QVariantMap wg{{"peers", QVariant::fromValue(peerList)}};
    if (!c.private_key.empty()) {
        wg["private-key"] = QString::fromStdString(c.private_key);
        wg["private-key-flags"] = uint(0);
    }
    if (c.listen_port)
        wg["listen-port"] = uint(c.listen_port);
    if (c.fwmark)
        wg["fwmark"] = uint(c.fwmark);
    if (c.mtu)
        wg["mtu"] = uint(c.mtu);
    if (c.table == vpn::kTableOff)
        wg["peer-routes"] = false;
    s["wireguard"] = wg;

    for (const bool v6 : {false, true}) {
        MapList addresses;
        for (const vpn::Address& a : c.addresses)
            if (a.v6 == v6)
                addresses.append({{"address", QString::fromStdString(a.ip)}, {"prefix", uint(a.prefix)}});
        QVariantMap ip;
        // No address of a family: none of it, and its DNS left out too.
        ip["method"] = addresses.isEmpty() ? "disabled" : "manual";
        if (!addresses.isEmpty()) {
            ip["address-data"] = QVariant::fromValue(addresses);
            const auto& dns = v6 ? c.dns6 : c.dns4;
            if (!v6) {
                QList<uint> list;
                for (const std::string& d : dns) {
                    uint n = 0;
                    inet_pton(AF_INET, d.c_str(), &n);
                    list.append(n);
                }
                if (!list.isEmpty())
                    ip["dns"] = QVariant::fromValue(list);
            } else {
                QList<QByteArray> list;
                for (const std::string& d : dns) {
                    QByteArray b(16, '\0');
                    inet_pton(AF_INET6, d.c_str(), b.data());
                    list.append(b);
                }
                if (!list.isEmpty())
                    ip["dns"] = QVariant::fromValue(list);
            }
            QStringList search;
            for (const std::string& d : c.dns_search)
                search.append(QString::fromStdString(d));
            // DNS without a search domain: every lookup through it.
            if (search.isEmpty() && !dns.empty())
                search.append("~");
            if (!search.isEmpty())
                ip["dns-search"] = search;
        }
        if (c.table >= 0)
            ip["route-table"] = uint(c.table);
        s[v6 ? "ipv6" : "ipv4"] = ip;
    }
    return s;
}

void Vpn::importFile(const QUrl& url) {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    std::vector<std::pair<std::string, std::string>> files;
    QString error;
    if (!read_configs(path, files, error)) {
        emit importFailed(error);
        return;
    }
    const vpn::Gathered g = vpn::gather(QFileInfo(path).fileName().toStdString(), files);
    if (g.tunnels.empty()) {
        emit importFailed(g.errors.empty() ? "Nothing to import." : QString::fromStdString(g.errors.front()));
        return;
    }
    for (const vpn::Tunnel& t : g.tunnels) {
        // The same name imported again replaces it (a new key, new servers).
        QString replacing;
        for (VpnTunnel* existing : tunnels_)
            if (existing->name() == QString::fromStdString(t.name) && stored_.contains(existing->uuid()))
                replacing = existing->path();
        add(t, replacing);
    }
}

void Vpn::add(const vpn::Tunnel& tunnel, const QString& replacing) {
    load();
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::vector<std::string> taken;
    QString interface;
    for (VpnTunnel* t : tunnels_) {
        const QJsonObject o = stored_[t->uuid()].toObject();
        if (t->path() == replacing) {
            uuid = t->uuid();
            interface = o["interface"].toString();
        } else if (o.contains("interface")) {
            taken.push_back(o["interface"].toString().toStdString());
        }
    }
    const QString name = QString::fromStdString(tunnel.name);
    if (interface.isEmpty())
        interface = QString::fromStdString(vpn::interface_name(tunnel.name, taken));

    // The first server: one in this country if there is one, as Mullvad
    // starts near you.
    QString first;
    std::vector<vpn::Peer> peers = tunnel.config.peers;
    if (!tunnel.servers.empty()) {
        const QString here = QLocale::territoryToCode(QLocale::system().territory()).toLower();
        std::vector<const vpn::Server*> near;
        for (const vpn::Server& s : tunnel.servers)
            if (QString::fromStdString(s.country) == here)
                near.push_back(&s);
        const vpn::Server* pick = near.empty() ? &tunnel.servers.front() : near[QRandomGenerator::global()->bounded(int(near.size()))];
        first = QString::fromStdString(pick->id);
        peers = {pick->peer};
    }
    const Settings settings = settingsFor(uuid, name, interface, tunnel.config, peers);

    QJsonArray servers;
    for (const vpn::Server& s : tunnel.servers) {
        VpnTunnel::Server named{s, {}, {}};
        if (!s.country.empty()) {
            named.countryName = country_name(QString::fromStdString(s.country));
            named.cityName = QString::fromStdString(s.city).toUpper();
        }
        servers.append(to_json(named));
    }
    vpn::Config kept = tunnel.config;
    kept.private_key.clear();
    kept.peers.clear();
    stored_[uuid] = QJsonObject{{"name", name}, {"interface", interface}, {"config", to_json(kept)},
                                {"server", first}, {"servers", servers}};
    save();

    QDBusMessage m = replacing.isEmpty() ? QDBusMessage::createMethodCall(kNM, kSettingsPath, kSettings, "AddConnection2")
                                         : QDBusMessage::createMethodCall(kNM, replacing, kConnection, "Update2");
    m << QVariant::fromValue(settings) << uint(1) << QVariantMap();  // to disk
    ++importing_;
    emit importingChanged();
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    const int count = int(tunnel.servers.size());
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this, uuid, name, count](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        --importing_;
        emit importingChanged();
        if (w->isError()) {
            stored_.remove(uuid);
            save();
            emit importFailed(w->error().message());
            return;
        }
        emit imported(name, count);
        reloadSoon();
        fetchNames();
    });
}

void Vpn::moveTo(VpnTunnel* t, const QString& server) {
    load();
    auto it = std::find_if(t->servers_.begin(), t->servers_.end(), [&](const VpnTunnel::Server& s) { return QString::fromStdString(s.server.id) == server; });
    if (it == t->servers_.end())
        return;
    const QJsonObject o = stored_[t->uuid()].toObject();
    // No key in it: NetworkManager keeps the secrets it has.
    const Settings settings = settingsFor(t->uuid(), t->name(), o["interface"].toString(), t->config_, {it->server.peer});
    QDBusMessage m = QDBusMessage::createMethodCall(kNM, t->path(), kConnection, "Update2");
    m << QVariant::fromValue(settings) << uint(1) << QVariantMap();
    t->switching_ = true;
    emit t->changed();
    auto* w = new QDBusPendingCallWatcher(bus().asyncCall(m), this);
    QPointer<VpnTunnel> tunnel(t);
    QObject::connect(w, &QDBusPendingCallWatcher::finished, this, [this, tunnel, server](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (!tunnel)
            return;
        if (w->isError()) {
            tunnel->fail(w->error().message());
            return;
        }
        tunnel->server_ = server;
        QJsonObject o = stored_[tunnel->uuid()].toObject();
        o["server"] = server;
        stored_[tunnel->uuid()] = o;
        save();
        tunnel->switching_ = false;
        emit tunnel->changed();
        emit changed();
        // On: onto the new server at once, without going down.
        if (tunnel->connected() && !tunnel->device_.isEmpty()) {
            QDBusMessage r = QDBusMessage::createMethodCall(kNM, tunnel->device_, kDevice, "Reapply");
            r << QVariant::fromValue(Settings()) << qulonglong(0) << uint(0);
            auto* rw = new QDBusPendingCallWatcher(bus().asyncCall(r), this);
            QObject::connect(rw, &QDBusPendingCallWatcher::finished, this, [this, tunnel](QDBusPendingCallWatcher* rw) {
                rw->deleteLater();
                if (rw->isError() && tunnel)
                    activate(tunnel);
            });
        } else {
            tunnel->start();
        }
    });
}

void Vpn::remember(VpnTunnel* t) {
    load();
    if (!stored_.contains(t->uuid()))
        return;
    QJsonObject o = stored_[t->uuid()].toObject();
    o["default"] = t->default_;
    QJsonArray recent;
    for (const std::string& r : t->recent_)
        recent.append(QString::fromStdString(r));
    o["recent"] = recent;
    stored_[t->uuid()] = o;
    save();
}

void Vpn::remove(VpnTunnel* t) {
    load();
    if (t->connected() || t->busy())
        deactivate(t);
    stored_.remove(t->uuid());
    save();
    bus().asyncCall(QDBusMessage::createMethodCall(kNM, t->path(), kConnection, "Delete"));
}

void Vpn::fetchNames() {
    // Mullvad's servers by hostname: their countries' and cities' names.
    bool any = false;
    for (const QJsonValue& v : stored_)
        for (const QJsonValue& s : v.toObject()["servers"].toArray())
            any = any || !s.toObject()["country"].toString().isEmpty();
    if (!any)
        return;
    if (!net_)
        net_ = new QNetworkAccessManager(this);
    QNetworkReply* reply = net_->get(QNetworkRequest(QUrl(kMullvadRelays)));
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QJsonArray relays = QJsonDocument::fromJson(reply->readAll()).array();
        if (reply->error() != QNetworkReply::NoError || relays.isEmpty())
            return;  // the codes stay
        QHash<QString, std::pair<QString, QString>> names;
        for (const QJsonValue& r : relays)
            names.insert(r["hostname"].toString(), {r["country_name"].toString(), r["city_name"].toString()});
        bool renamed = false;
        for (const QString& uuid : stored_.keys()) {
            QJsonObject t = stored_[uuid].toObject();
            QJsonArray servers = t["servers"].toArray();
            for (qsizetype i = 0; i < servers.size(); ++i) {
                QJsonObject s = servers[i].toObject();
                const auto it = names.constFind(s["id"].toString());
                if (it == names.constEnd() || (s["countryName"] == it->first && s["cityName"] == it->second))
                    continue;
                s["countryName"] = it->first;
                s["cityName"] = it->second;
                servers[i] = s;
                renamed = true;
            }
            t["servers"] = servers;
            stored_[uuid] = t;
        }
        if (renamed) {
            save();
            reloadSoon();
        }
    });
}

bool Vpn::load() {
    const QFileInfo info(file());
    if (!info.exists() || info.lastModified() == storedTime_)
        return false;
    QFile f(file());
    if (!f.open(QIODevice::ReadOnly))
        return false;
    storedTime_ = info.lastModified();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    stored_ = o["tunnels"].toObject();
    last_ = o["last"].toString();
    return true;
}

void Vpn::save() {
    QDir().mkpath(QFileInfo(file()).path());
    QSaveFile f(file());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{"last", last_}, {"tunnels", stored_}}).toJson(QJsonDocument::Compact));
    if (f.commit())
        storedTime_ = QFileInfo(file()).lastModified();
}

} // namespace atrium
