#include "daemon.hpp"

#include "avahi_qt.hpp"
#include "compositor.hpp"
#include "crypto.hpp"

#include <avahi-common/domain.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>

#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QSysInfo>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cstring>

Q_LOGGING_CATEGORY(lc, "atrium.phonelink")

namespace atrium::phonelink {

namespace {

const QString kSetting = QStringLiteral("phone.audio");
constexpr qint64 kRetryMs = 3000;

QString hex(std::string_view b) { return QString::fromLatin1(QByteArray(b.data(), qsizetype(b.size())).toHex()); }

std::string unhex(const QString& h) { return QByteArray::fromHex(h.toLatin1()).toStdString(); }

std::string be16(std::uint16_t v) { return {char(v >> 8), char(v)}; }

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

} // namespace

// --- Status ------------------------------------------------------------------

Status::Status(Daemon& d) : QObject(&d), d_(d) {}

void Status::PairWith(const QString& id) { d_.pairWith(id.toLower()); }
void Status::CancelPairing() { d_.cancelPairing(); }
void Status::Accept() { d_.answer(true); }
void Status::Reject() { d_.answer(false); }
void Status::Connect(const QString& id) { d_.connectPhone(id.toLower()); }
void Status::Disconnect(const QString& id) { d_.disconnectPhone(id.toLower()); }
void Status::SetAutoConnect(const QString& id, bool on) { d_.setAutoConnect(id.toLower(), on); }
void Status::Forget(const QString& id) { d_.forget(id.toLower()); }

// --- Daemon ------------------------------------------------------------------

Daemon::Daemon(QObject* parent)
    : QObject(parent), status_(new Status(*this)),
      mpris_(new Mpris(qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath()) + "/atrium-phonelink", this)) {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    dir_ = state + "/atrium/phonelink";
    QDir().mkpath(dir_);
    QFile::setPermissions(dir_, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QFile idFile(dir_ + "/id");
    if (idFile.open(QIODevice::ReadOnly) && idFile.size() == qint64(kIdSize)) {
        me_.id = idFile.readAll().toStdString();
    } else {
        me_.id = random(kIdSize);
        QSaveFile f(dir_ + "/id");
        if (f.open(QIODevice::WriteOnly)) {
            f.write(me_.id.data(), qint64(me_.id.size()));
            f.commit();
        }
    }
    me_.name = QSysInfo::machineHostName().toStdString();
    loadPaired();

    // How far behind the phone playback runs: Wi-Fi's jitter against delay.
    const int ms = qEnvironmentVariableIntValue("ATRIUM_PHONELINK_TARGET_MS");
    target_ = std::uint32_t(kRate / 1000 * (ms > 0 ? ms : 40));

    qDBusRegisterMetaType<QList<QVariantMap>>();
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/atrium/PhoneLink"), status_,
                                                 QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals |
                                                     QDBusConnection::ExportAllSlots);

    retry_.setInterval(1000);
    connect(&retry_, &QTimer::timeout, this, &Daemon::reconcile);

    Compositor* compositor = Compositor::instance();
    // ATRIUM_PHONELINK=on: on whatever the setting says (a session that
    // predates it, or trying it out).
    const bool forced = qEnvironmentVariable("ATRIUM_PHONELINK") == QLatin1String("on");
    const auto follow = [this, compositor, forced] { setEnabled(forced || compositor->setting(kSetting, false).toBool()); };
    connect(compositor, &Compositor::settingsChanged, this, follow);
    follow();
    updateStatus();
}

Daemon::~Daemon() {
    for (Conn* c : std::as_const(conns_))
        delete c;
    conns_.clear();
    if (avahi_)
        avahi_client_free(avahi_);  // frees the browser and resolvers too
}

void Daemon::setEnabled(bool on) {
    if (on == enabled_)
        return;
    enabled_ = on;
    qCInfo(lc) << (on ? "on" : "off");
    if (on) {
        startAvahi();
        retry_.start();
    } else {
        retry_.stop();
        pairingWith_.clear();
        // Closing gives the phone its sound back.
        for (Conn* c : conns_.values())
            drop(c, {});
        forgetServices();
        if (avahi_) {
            avahi_client_free(avahi_);
            avahi_ = nullptr;
        }
    }
    updateStatus();
}

// --- discovery ---------------------------------------------------------------

void Daemon::startAvahi() {
    if (avahi_)
        return;
    int error = 0;
    // Waits for the daemon when it isn't running yet (or restarts).
    avahi_ = avahi_client_new(qtAvahiPoll(), AVAHI_CLIENT_NO_FAIL, &Daemon::clientEvent, this, &error);
    if (!avahi_)
        qCWarning(lc) << "avahi:" << avahi_strerror(error);
}

void Daemon::clientEvent(AvahiClient* c, AvahiClientState state, void* self) {
    auto* d = static_cast<Daemon*>(self);
    if (state == AVAHI_CLIENT_S_RUNNING && !d->browser_) {
        const std::string type(kServiceType);
        d->browser_ = avahi_service_browser_new(c, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, type.c_str(), nullptr,
                                                AvahiLookupFlags(0), &Daemon::browseEvent, d);
        if (!d->browser_)
            qCWarning(lc) << "avahi browse:" << avahi_strerror(avahi_client_errno(c));
    } else if (state == AVAHI_CLIENT_FAILURE || state == AVAHI_CLIENT_CONNECTING) {
        // The daemon went away; its browsers with it.
        d->forgetServices();
        d->updateStatus();
    }
}

void Daemon::forgetServices() {
    for (Service& s : services_)
        if (s.txt)
            avahi_record_browser_free(s.txt);
    if (browser_)
        avahi_service_browser_free(browser_);
    browser_ = nullptr;
    services_.clear();
    serviceIds_.clear();
}

void Daemon::browseEvent(AvahiServiceBrowser* b, AvahiIfIndex iface, AvahiProtocol proto, AvahiBrowserEvent event,
                         const char* name, const char* type, const char* domain, AvahiLookupResultFlags, void* self) {
    auto* d = static_cast<Daemon*>(self);
    if (event == AVAHI_BROWSER_NEW) {
        // The phone only speaks IPv4 announcements; take what resolves.
        avahi_service_resolver_new(avahi_service_browser_get_client(b), iface, proto, name, type, domain,
                                   AVAHI_PROTO_UNSPEC, AvahiLookupFlags(0), &Daemon::resolveEvent, d);
    } else if (event == AVAHI_BROWSER_REMOVE) {
        const QString id = d->serviceIds_.take(QString::fromUtf8(name));
        if (!id.isEmpty()) {
            qCInfo(lc) << "gone:" << name;
            const Service s = d->services_.take(id);
            if (s.txt)
                avahi_record_browser_free(s.txt);
            d->updateStatus();
        }
    }
}

void Daemon::resolveEvent(AvahiServiceResolver* r, AvahiIfIndex iface, AvahiProtocol proto, AvahiResolverEvent event,
                          const char* name, const char*, const char* domain, const char*, const AvahiAddress* a,
                          uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* self) {
    auto* d = static_cast<Daemon*>(self);
    if (event == AVAHI_RESOLVER_FOUND && a) {
        QString id;
        if (AvahiStringList* l = avahi_string_list_find(txt, "id")) {
            char *key = nullptr, *value = nullptr;
            if (avahi_string_list_get_pair(l, &key, &value, nullptr) == 0 && value)
                id = QString::fromLatin1(value).toLower();
            avahi_free(key);
            avahi_free(value);
        }
        char text[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(text, sizeof text, a);
        if (id.size() == int(kIdSize * 2)) {
            Service s{QString::fromUtf8(name), QHostAddress(QString::fromLatin1(text)), port, iface, proto, domain};
            readTxt(s, txt);
            auto old = d->services_.constFind(id);
            const bool moved = old == d->services_.cend() || old->address != s.address || old->port != s.port;
            if (old != d->services_.cend()) {
                s.txt = old->txt;
            } else {
                // The TXT record changes as the phone's user pairs or calls.
                char full[AVAHI_DOMAIN_NAME_MAX];
                const std::string type(kServiceType);
                if (avahi_service_name_join(full, sizeof full, name, type.c_str(), domain) == 0)
                    s.txt = avahi_record_browser_new(avahi_service_resolver_get_client(r), iface, proto, full,
                                                     AVAHI_DNS_CLASS_IN, AVAHI_DNS_TYPE_TXT, AvahiLookupFlags(0),
                                                     &Daemon::txtEvent, d);
            }
            if (moved)
                qCInfo(lc) << "found:" << s.name << "at" << s.address.toString() << port;
            d->services_[id] = s;
            d->serviceIds_[s.name] = id;
            if (moved)
                d->nextTry_.remove(id);  // somewhere new: try now
            d->reconcile();
            d->updateStatus();
        }
    }
    avahi_service_resolver_free(r);
}

void Daemon::txtEvent(AvahiRecordBrowser*, AvahiIfIndex, AvahiProtocol, AvahiBrowserEvent event, const char* name,
                      uint16_t, uint16_t, const void* rdata, size_t size, AvahiLookupResultFlags, void* self) {
    // The new record comes before the old one's removal: only NEW counts.
    if (event != AVAHI_BROWSER_NEW)
        return;
    auto* d = static_cast<Daemon*>(self);
    // "<instance>.<type>.<domain>": the instance is the browsed name.
    QString id;
    for (auto it = d->serviceIds_.cbegin(); it != d->serviceIds_.cend(); ++it) {
        char full[AVAHI_DOMAIN_NAME_MAX];
        const std::string type(kServiceType);
        const auto s = d->services_.constFind(it.value());
        if (s != d->services_.cend() &&
            avahi_service_name_join(full, sizeof full, it.key().toUtf8().constData(), type.c_str(),
                                    s->domain.constData()) == 0 &&
            avahi_domain_equal(full, name))
            id = it.value();
    }
    auto it = d->services_.find(id);
    AvahiStringList* txt = nullptr;
    if (it == d->services_.end() || avahi_string_list_parse(rdata, size, &txt) < 0)
        return;
    const QString call = it->call;
    readTxt(*it, txt);
    avahi_string_list_free(txt);
    // The phone's user asked this PC to connect.
    if (it->call != call && it->call == hex(d->me_.id) && d->paired_.contains(id)) {
        qCInfo(lc) << it->name << "asks to connect";
        d->manual_[id] = true;
        d->forgotBy_.remove(id);
        d->nextTry_.remove(id);
        d->reconcile();
    }
    d->updateStatus();
}

void Daemon::readTxt(Service& s, AvahiStringList* txt) {
    const auto value = [txt](const char* key) {
        QString v;
        if (AvahiStringList* l = avahi_string_list_find(txt, key)) {
            char *k = nullptr, *value = nullptr;
            if (avahi_string_list_get_pair(l, &k, &value, nullptr) == 0 && value)
                v = QString::fromLatin1(value).toLower();
            avahi_free(k);
            avahi_free(value);
        }
        return v;
    };
    s.pairing = value("pair") == QLatin1String("1");
    s.call = value("call");
}

void Daemon::resolve(const Service& s) {
    if (!avahi_)
        return;
    const std::string type(kServiceType);
    avahi_service_resolver_new(avahi_, s.iface, s.protocol, s.name.toUtf8().constData(), type.c_str(),
                               s.domain.constData(), AVAHI_PROTO_UNSPEC, AvahiLookupFlags(0), &Daemon::resolveEvent,
                               this);
}

// --- connections -------------------------------------------------------------

// A paired phone this PC should be connected to whenever it's around.
bool Daemon::wants(const QString& id) const {
    auto it = paired_.constFind(id);
    return it != paired_.cend() && !forgotBy_.contains(id) && manual_.value(id, it->autoConnect);
}

void Daemon::reconcile() {
    if (!enabled_)
        return;
    for (auto it = services_.cbegin(); it != services_.cend(); ++it) {
        const QString& id = it.key();
        if (conns_.contains(id) || nextTry_.value(id) > now())
            continue;
        if (wants(id) || id == pairingWith_)
            connectTo(id, it.value());
    }
}

void Daemon::connectTo(const QString& id, const Service& s) {
    nextTry_[id] = now() + kRetryMs;
    auto* c = new Conn;
    c->id = id;
    c->name = s.name;
    c->socket = std::make_unique<QTcpSocket>();
    c->link = std::make_unique<Link>(
        Role::Pc, me_,
        [this](const std::string& peer) -> std::optional<std::string> {
            auto it = paired_.constFind(hex(peer));
            if (it == paired_.cend())
                return std::nullopt;
            return it->key;
        },
        [](std::size_t n) { return random(n); });
    c->link->setPairing(id == pairingWith_);
    conns_[id] = c;
    QTcpSocket* sock = c->socket.get();
    connect(sock, &QTcpSocket::connected, this, [this, c, sock] {
        const int fd = int(sock->socketDescriptor());
        int on = 1, idle = 5, interval = 2, count = 3;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
        sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        apply(c, c->link->start());
    });
    connect(sock, &QTcpSocket::readyRead, this, [this, c, sock] {
        const QByteArray b = sock->readAll();
        apply(c, c->link->received(std::string_view(b.constData(), std::size_t(b.size()))));
    });
    connect(sock, &QTcpSocket::errorOccurred, this, [this, c, sock] { drop(c, sock->errorString()); });
    qCInfo(lc) << "connecting to" << s.name;
    sock->connectToHost(s.address, s.port);
    updateStatus();
}

void Daemon::apply(Conn* c, std::vector<Event> events) {
    for (Event& e : events) {
        if (c->closed)
            return;
        switch (e.kind) {
        case Event::Kind::Send:
            c->socket->write(e.bytes.data(), qint64(e.bytes.size()));
            break;
        case Event::Kind::PairCode:
            if (confirming_ && confirming_ != c)
                apply(confirming_, confirming_->link->reject());
            confirming_ = c;
            code_ = QString::fromStdString(e.text);
            c->name = QString::fromStdString(c->link->peer().name);
            qCInfo(lc) << "pairing with" << c->name;
            break;
        case Event::Kind::Paired: {
            const QString id = hex(c->link->peer().id);
            paired_[id] = {e.bytes, QString::fromStdString(c->link->peer().name)};
            savePaired();
            confirming_ = nullptr;
            code_.clear();
            pairingWith_.clear();
            pairFailed_.clear();
            manual_.remove(id);
            forgotBy_.remove(id);
            qCInfo(lc) << "paired with" << paired_[id].name;
            break;
        }
        case Event::Kind::Ready: {
            c->name = QString::fromStdString(c->link->peer().name);
            auto it = paired_.find(c->id);
            if (it != paired_.end() && it->name != c->name) {
                it->name = c->name;
                savePaired();
            }
            sockaddr_in6 phone{};
            phone.sin6_family = AF_INET6;
            phone.sin6_port = htons(c->socket->peerPort());
            const QHostAddress peer = c->socket->peerAddress();
            bool v4 = false;
            const quint32 ip4 = peer.toIPv4Address(&v4);
            if (v4) {
                phone.sin6_addr.s6_addr[10] = phone.sin6_addr.s6_addr[11] = 0xff;
                const quint32 n = htonl(ip4);
                std::memcpy(&phone.sin6_addr.s6_addr[12], &n, 4);
            } else {
                const Q_IPV6ADDR a = peer.toIPv6Address();
                std::memcpy(&phone.sin6_addr, &a, 16);
                phone.sin6_scope_id = peer.scopeId().toUInt();
            }
            c->playback = std::make_unique<Playback>(c->link->datagramKey(), c->nackCounter, c->name.toStdString(),
                                                     target_, phone);
            if (!c->playback->ok()) {
                c->failed = QStringLiteral("can't play the sound here");
                break;
            }
            qCInfo(lc) << "ready with" << c->name << "- asking for its sound";
            apply(c, c->link->message(Type::Want, std::string(1, char(manual_.value(c->id, false) ? 1 : 0))));
            apply(c, c->link->message(Type::AudioStart, be16(c->playback->port()) + be16(kPacketFrames)));
            break;
        }
        case Event::Kind::Message:
            message(c, e.type, e.bytes);
            break;
        case Event::Kind::Close:
            drop(c, QString::fromStdString(e.text));
            return;
        }
    }
    updateStatus();
}

void Daemon::message(Conn* c, Type type, const std::string& body) {
    if (type == Type::Disconnect) {
        // Its user's: not back until asked.
        qCInfo(lc) << c->name << "disconnected this PC";
        manual_[c->id] = false;
        drop(c, {});
        return;
    }
    if (type == Type::Media) {
        if (std::optional<Media> m = unpackMedia(body)) {
            c->media = std::move(*m);
            updateMedia();
        }
        return;
    }
    if (type != Type::AudioState || body.empty())
        return;
    const auto state = AudioState(std::uint8_t(body[0]));
    const QString why = QString::fromUtf8(body.data() + 1, qsizetype(body.size() - 1));
    c->streaming = state == AudioState::Streaming;
    c->failed = state == AudioState::Failed ? why : QString();
    qCInfo(lc) << c->name << "audio:" << int(state) << why;
}

void Daemon::drop(Conn* c, const QString& why) {
    if (c->closed)
        return;
    c->closed = true;
    if (why == QLatin1String(kNotKnown.data(), qsizetype(kNotKnown.size()))) {
        // Forgotten on the phone: no use trying again until paired again.
        qCInfo(lc) << c->name << "forgot this PC";
        forgotBy_.insert(c->id);
    } else if (!why.isEmpty()) {
        qCInfo(lc) << "closed" << c->name << ":" << why;
        // The phone may have moved (a restart, a new address).
        if (auto it = services_.constFind(c->id); it != services_.cend() && enabled_)
            resolve(*it);
    }
    if (c->id == pairingWith_) {
        // Paired or not, this try is over: no retrying into a new code.
        pairingWith_.clear();
        if (!paired_.contains(c->id) && !why.isEmpty()) {
            pairFailed_ = c->id;
            pairError_ = why == QLatin1String("the phone isn't pairing now")
                             ? QStringLiteral("Press Pair on the phone first, then here")
                             : why;
        }
    }
    if (confirming_ == c) {
        confirming_ = nullptr;
        code_.clear();
    }
    conns_.remove(c->id);
    updateMedia();
    c->socket->disconnect(this);
    c->socket->abort();
    // Not from inside the socket's own signal.
    QTimer::singleShot(0, this, [c] { delete c; });
    updateStatus();
}

// The phone whose sound plays here, else any with a media session.
void Daemon::updateMedia() {
    Conn* best = nullptr;
    for (Conn* c : std::as_const(conns_))
        if (c->link->ready() && c->media.status != MediaStatus::None && (!best || (c->streaming && !best->streaming)))
            best = c;
    if (!best) {
        mpris_->clear();
        return;
    }
    mpris_->set(best->name, best->media, [this, id = best->id](MediaCommand cmd, std::uint32_t position) {
        if (Conn* c = conns_.value(id); c && c->link->ready())
            apply(c, c->link->message(Type::MediaCommand, packMediaCommand(cmd, position)));
    });
}

// --- pairing -----------------------------------------------------------------

void Daemon::pairWith(const QString& id) {
    if (!enabled_ || paired_.contains(id) || !services_.contains(id) || confirming_)
        return;
    if (Conn* c = conns_.value(pairingWith_))
        drop(c, {});
    pairingWith_ = id;
    pairFailed_.clear();
    nextTry_.remove(id);
    reconcile();
    updateStatus();
}

void Daemon::cancelPairing() {
    if (confirming_)
        apply(confirming_, confirming_->link->reject());
    if (Conn* c = conns_.value(pairingWith_))
        drop(c, {});
    pairingWith_.clear();
    updateStatus();
}

void Daemon::answer(bool accept) {
    if (!confirming_)
        return;
    Conn* c = confirming_;
    apply(c, accept ? c->link->accept() : c->link->reject());
}

void Daemon::connectPhone(const QString& id) {
    if (!paired_.contains(id))
        return;
    manual_[id] = true;
    forgotBy_.remove(id);
    nextTry_.remove(id);
    reconcile();
    updateStatus();
}

void Daemon::disconnectPhone(const QString& id) {
    if (!paired_.contains(id))
        return;
    manual_[id] = false;
    if (Conn* c = conns_.value(id))
        drop(c, {});  // the phone takes its sound back
    updateStatus();
}

void Daemon::setAutoConnect(const QString& id, bool on) {
    auto it = paired_.find(id);
    if (it == paired_.end())
        return;
    it->autoConnect = on;
    // The switch is the newest word: it ends a Connect or Disconnect.
    // Turning it off leaves a connection there as it is.
    if (on || !conns_.contains(id))
        manual_.remove(id);
    else
        manual_[id] = true;
    savePaired();
    reconcile();
    updateStatus();
}

void Daemon::forget(const QString& id) {
    if (!paired_.remove(id))
        return;
    savePaired();
    manual_.remove(id);
    forgotBy_.remove(id);
    if (Conn* c = conns_.value(id))
        drop(c, {});
    updateStatus();
}

// One line each: hex id, hex key, name. The ones that don't connect by
// themselves are listed in `manual`, one id a line.
void Daemon::loadPaired() {
    QFile f(dir_ + "/paired");
    if (!f.open(QIODevice::ReadOnly))
        return;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const QStringList p = line.split(' ');
        if (p.size() >= 2 && p[0].size() == int(kIdSize * 2))
            paired_[p[0]] = {unhex(p[1]), line.section(' ', 2)};
    }
    QFile m(dir_ + "/manual");
    if (m.open(QIODevice::ReadOnly))
        while (!m.atEnd())
            if (auto it = paired_.find(QString::fromLatin1(m.readLine()).trimmed()); it != paired_.end())
                it->autoConnect = false;
}

void Daemon::savePaired() {
    QSaveFile f(dir_ + "/paired"), m(dir_ + "/manual");
    if (!f.open(QIODevice::WriteOnly) || !m.open(QIODevice::WriteOnly))
        return;
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    for (auto it = paired_.cbegin(); it != paired_.cend(); ++it) {
        f.write((it.key() + ' ' + hex(it->key) + ' ' + it->name + '\n').toUtf8());
        if (!it->autoConnect)
            m.write((it.key() + '\n').toLatin1());
    }
    f.commit();
    m.commit();
}

// --- status ------------------------------------------------------------------

void Daemon::updateStatus() {
    QList<QVariantMap> phones, nearby;
    for (auto it = paired_.cbegin(); it != paired_.cend(); ++it) {
        const QString& id = it.key();
        QString state, error;
        if (Conn* c = conns_.value(id); c && c->link->ready()) {
            error = c->failed;
            state = !c->failed.isEmpty() ? QStringLiteral("failed")
                    : c->streaming       ? QStringLiteral("streaming")
                                         : QStringLiteral("connected");
        } else if (conns_.contains(id)) {
            state = QStringLiteral("connecting");
        } else if (forgotBy_.contains(id)) {
            state = QStringLiteral("forgot");
        } else if (!services_.contains(id)) {
            state = QStringLiteral("away");
        } else if (!wants(id)) {
            state = QStringLiteral("disconnected");
        } else {
            state = QStringLiteral("connecting");  // between tries
        }
        phones.push_back({{QStringLiteral("id"), id},
                          {QStringLiteral("name"), it->name},
                          {QStringLiteral("auto"), it->autoConnect},
                          {QStringLiteral("state"), state},
                          {QStringLiteral("error"), error}});
    }
    for (auto it = services_.cbegin(); it != services_.cend(); ++it) {
        const QString& id = it.key();
        if (paired_.contains(id))
            continue;
        nearby.push_back({{QStringLiteral("id"), id},
                          {QStringLiteral("name"), it->name},
                          {QStringLiteral("ready"), it->pairing},
                          {QStringLiteral("state"), id == pairingWith_ ? QStringLiteral("pairing") : QString()},
                          {QStringLiteral("error"), id == pairFailed_ ? pairError_ : QString()}});
    }
    const QString state = !enabled_ ? QStringLiteral("off") : confirming_ ? QStringLiteral("confirm") : QStringLiteral("on");
    const QString phone = confirming_ ? confirming_->name : QString();
    const QString code = confirming_ ? code_ : QString();
    Status& s = *status_;
    if (s.state == state && s.phone == phone && s.code == code && s.phones == phones && s.nearby == nearby)
        return;
    s.state = state;
    s.phone = phone;
    s.code = code;
    s.phones = phones;
    s.nearby = nearby;
    emit s.StatusChanged();
}

} // namespace atrium::phonelink
