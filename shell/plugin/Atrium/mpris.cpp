#include "mpris.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace atrium {

namespace {

const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kRoot = QStringLiteral("org.mpris.MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");

QVariant plain(const QVariant& v) {
    if (v.canConvert<QDBusVariant>())
        return plain(v.value<QDBusVariant>().variant());
    if (v.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::MapType)
            return qdbus_cast<QVariantMap>(arg);
        if (arg.currentType() == QDBusArgument::ArrayType)
            return qdbus_cast<QStringList>(arg);
    }
    return v;
}

} // namespace

// --- MprisPlayer -------------------------------------------------------------

MprisPlayer::MprisPlayer(const QString& service, QObject* parent) : QObject(parent), service_(service) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(service, kPath, kProps, "PropertiesChanged", this,
                SLOT(propertiesChanged(QString, QVariantMap, QStringList)));
    bus.connect(service, kPath, kPlayer, "Seeked", this, SLOT(seeked(qlonglong)));
    tick_.setInterval(1000);
    connect(&tick_, &QTimer::timeout, this, &MprisPlayer::positionChanged);
    since_.start();
    fetch(kRoot);
    fetch(kPlayer);
}

void MprisPlayer::fetch(const QString& interface) {
    QDBusMessage m = QDBusMessage::createMethodCall(service_, kPath, kProps, "GetAll");
    m << interface;
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, interface] {
        w->deleteLater();
        QDBusPendingReply<QVariantMap> r = *w;
        if (r.isError())
            return;
        apply(r.value());
        if (interface == kPlayer)
            fetchPosition();
    });
}

void MprisPlayer::fetchPosition() {
    QDBusMessage m = QDBusMessage::createMethodCall(service_, kPath, kProps, "Get");
    m << kPlayer << QStringLiteral("Position");
    auto* w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        QDBusPendingReply<QDBusVariant> r = *w;
        if (r.isError())
            return;
        positionUs_ = r.value().variant().toLongLong();
        since_.restart();
        emit positionChanged();
    });
}

void MprisPlayer::apply(const QVariantMap& props) {
    // Where the position is now, before the rate or status changes it.
    positionUs_ = qlonglong(position() * 1e6);
    since_.restart();
    const QString oldTrack = trackId_;
    for (auto it = props.begin(); it != props.end(); ++it) {
        const QString& k = it.key();
        const QVariant v = plain(it.value());
        if (k == "Identity") identity_ = v.toString();
        else if (k == "DesktopEntry") desktopEntry_ = v.toString();
        else if (k == "PlaybackStatus") status_ = v.toString();
        else if (k == "Rate") rate_ = v.toDouble();
        else if (k == "CanControl") canControl_ = v.toBool();
        else if (k == "CanPlay") canPlay_ = v.toBool();
        else if (k == "CanPause") canPause_ = v.toBool();
        else if (k == "CanGoNext") canNext_ = v.toBool();
        else if (k == "CanGoPrevious") canPrevious_ = v.toBool();
        else if (k == "CanSeek") canSeek_ = v.toBool();
        else if (k == "Position") positionUs_ = v.toLongLong();
        else if (k == "Metadata") {
            const QVariantMap md = v.toMap();
            title_ = plain(md.value("xesam:title")).toString();
            const QVariant artist = plain(md.value("xesam:artist"));
            artist_ = artist.canConvert<QStringList>() ? artist.toStringList().join(", ") : artist.toString();
            album_ = plain(md.value("xesam:album")).toString();
            artUrl_ = plain(md.value("mpris:artUrl")).toString();
            lengthUs_ = plain(md.value("mpris:length")).toLongLong();
            const QVariant id = plain(md.value("mpris:trackid"));
            trackId_ = id.canConvert<QDBusObjectPath>() ? id.value<QDBusObjectPath>().path() : id.toString();
        }
    }
    if (isPlaying())
        tick_.start();
    else
        tick_.stop();
    emit changed();
    // A new track starts somewhere the player knows.
    if (trackId_ != oldTrack)
        fetchPosition();
    else
        emit positionChanged();
}

void MprisPlayer::propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList&) {
    if (interface == kRoot || interface == kPlayer)
        apply(changed);
}

void MprisPlayer::seeked(qlonglong us) {
    positionUs_ = us;
    since_.restart();
    emit positionChanged();
}

double MprisPlayer::position() const {
    double us = double(positionUs_);
    if (isPlaying())
        us += since_.elapsed() * 1000.0 * rate_;
    if (lengthUs_ > 0)
        us = std::min(us, double(lengthUs_));
    return std::max(0.0, us / 1e6);
}

void MprisPlayer::setPosition(double seconds) {
    if (!canSeek() || trackId_.isEmpty())
        return;
    const qlonglong us = qlonglong(seconds * 1e6);
    call("SetPosition", {QVariant::fromValue(QDBusObjectPath(trackId_)), us});
    positionUs_ = us;
    since_.restart();
    emit positionChanged();
}

void MprisPlayer::call(const QString& method, const QVariantList& args) {
    QDBusMessage m = QDBusMessage::createMethodCall(service_, kPath, kPlayer, method);
    m.setArguments(args);
    QDBusConnection::sessionBus().asyncCall(m);
}

void MprisPlayer::togglePlaying() {
    call("PlayPause");
}

void MprisPlayer::next() {
    call("Next");
}

void MprisPlayer::previous() {
    call("Previous");
}

// --- Mpris -------------------------------------------------------------------

Mpris* Mpris::instance() {
    static auto* self = new Mpris;
    return self;
}

Mpris::Mpris() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged", this,
                SLOT(nameOwnerChanged(QString, QString, QString)));
    for (const QString& name : bus.interface()->registeredServiceNames().value())
        if (name.startsWith(kRoot + "."))
            add(name);
}

void Mpris::nameOwnerChanged(const QString& name, const QString& before, const QString& after) {
    if (!name.startsWith(kRoot + "."))
        return;
    if (!before.isEmpty())
        remove(name);
    if (!after.isEmpty())
        add(name);
}

void Mpris::add(const QString& service) {
    auto* p = new MprisPlayer(service, this);
    connect(p, &MprisPlayer::changed, this, &Mpris::activeChanged);
    players_.append(p);
    emit playersChanged();
    emit activeChanged();
}

void Mpris::remove(const QString& service) {
    for (qsizetype i = 0; i < players_.size(); ++i) {
        if (players_[i]->service() == service) {
            players_.takeAt(i)->deleteLater();
            emit playersChanged();
            emit activeChanged();
            return;
        }
    }
}

QList<QObject*> Mpris::players() const {
    return {players_.begin(), players_.end()};
}

void Mpris::setChosen(QObject* p) {
    auto* player = qobject_cast<MprisPlayer*>(p);
    if (player == chosen_)
        return;
    chosen_ = player;
    emit activeChanged();
}

QObject* Mpris::active() const {
    for (MprisPlayer* p : players_)
        if (p->isPlaying())
            return p;
    return players_.isEmpty() ? nullptr : players_.first();
}

} // namespace atrium
