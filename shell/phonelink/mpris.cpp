#include "mpris.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QUrl>

namespace atrium::phonelink {

namespace {

const QString kService = QStringLiteral("org.mpris.MediaPlayer2.atrium_phone");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");

QString qs(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }

} // namespace

// --- org.mpris.MediaPlayer2 ----------------------------------------------------

MprisRoot::MprisRoot(Mpris& m) : QDBusAbstractAdaptor(&m), m_(m) {}

QString MprisRoot::identity() const { return m_.phone_.isEmpty() ? QStringLiteral("Phone") : m_.phone_; }

// --- org.mpris.MediaPlayer2.Player ---------------------------------------------

MprisPlayer::MprisPlayer(Mpris& m) : QDBusAbstractAdaptor(&m), m_(m) {}

QString MprisPlayer::playbackStatus() const {
    switch (m_.media_.status) {
    case MediaStatus::Playing:
        return QStringLiteral("Playing");
    case MediaStatus::Paused:
        return QStringLiteral("Paused");
    default:
        return QStringLiteral("Stopped");
    }
}

QVariantMap MprisPlayer::metadata() const {
    const Media& md = m_.media_;
    QVariantMap out{
        {QStringLiteral("mpris:trackid"),
         QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/atrium/phone/track/%1").arg(m_.track_)))},
        {QStringLiteral("xesam:title"), qs(md.title)},
    };
    if (!md.artist.empty())
        out[QStringLiteral("xesam:artist")] = QStringList{qs(md.artist)};
    if (!md.album.empty())
        out[QStringLiteral("xesam:album")] = qs(md.album);
    if (md.duration)
        out[QStringLiteral("mpris:length")] = qlonglong(md.duration) * 1000;
    if (!m_.artUrl_.isEmpty())
        out[QStringLiteral("mpris:artUrl")] = m_.artUrl_;
    return out;
}

qlonglong MprisPlayer::position() const { return qlonglong(m_.positionMs()) * 1000; }
bool MprisPlayer::canGoNext() const { return m_.media_.actions & CanNext; }
bool MprisPlayer::canGoPrevious() const { return m_.media_.actions & CanPrevious; }
bool MprisPlayer::canPlay() const { return m_.media_.actions & CanPlay; }
bool MprisPlayer::canPause() const { return m_.media_.actions & CanPause; }
bool MprisPlayer::canSeek() const { return (m_.media_.actions & CanSeek) && m_.media_.duration; }

void MprisPlayer::Next() { m_.send(MediaCommand::Next); }
void MprisPlayer::Previous() { m_.send(MediaCommand::Previous); }
void MprisPlayer::Pause() { m_.send(MediaCommand::Pause); }
void MprisPlayer::PlayPause() { m_.send(MediaCommand::PlayPause); }
void MprisPlayer::Stop() { m_.send(MediaCommand::Stop); }
void MprisPlayer::Play() { m_.send(MediaCommand::Play); }

void MprisPlayer::Seek(qlonglong offset) {
    if (!canSeek())
        return;
    const qlonglong to = std::clamp(qlonglong(m_.positionMs()) + offset / 1000, 0LL, qlonglong(m_.media_.duration));
    m_.send(MediaCommand::Seek, std::uint32_t(to));
}

void MprisPlayer::SetPosition(const QDBusObjectPath&, qlonglong position) {
    if (!canSeek() || position < 0 || position / 1000 > qlonglong(m_.media_.duration))
        return;
    m_.send(MediaCommand::Seek, std::uint32_t(position / 1000));
}

// --- Mpris ---------------------------------------------------------------------

Mpris::Mpris(QString runtimeDir, QObject* parent) : QObject(parent), runtime_(std::move(runtimeDir)) {
    new MprisRoot(*this);
    player_ = new MprisPlayer(*this);
    QDir().mkpath(runtime_);
}

Mpris::~Mpris() {
    clear();
}

std::uint32_t Mpris::positionMs() const {
    std::uint64_t p = media_.position;
    if (media_.status == MediaStatus::Playing && since_.isValid())
        p += std::uint64_t(since_.elapsed());
    if (media_.duration)
        p = std::min<std::uint64_t>(p, media_.duration);
    return std::uint32_t(p);
}

void Mpris::send(MediaCommand c, std::uint32_t position) {
    if (command_)
        command_(c, position);
}

void Mpris::set(const QString& phone, const Media& media, Command command) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    command_ = std::move(command);
    if (media.status == MediaStatus::None) {
        if (registered_) {
            bus.unregisterObject(kPath);
            bus.unregisterService(kService);
            registered_ = false;
        }
        media_ = {};
        if (!artUrl_.isEmpty())
            QFile::remove(QUrl(artUrl_).toLocalFile());
        artUrl_.clear();
        return;
    }

    const Media old = media_;
    const bool newTrack = old.title != media.title || old.artist != media.artist || old.album != media.album;
    // Where the phone would be now, had nothing jumped.
    const std::int64_t expected = std::int64_t(positionMs());
    phone_ = phone;
    media_ = media;
    since_.start();
    if (newTrack)
        ++track_;
    if (old.art != media.art)
        artUrl_ = writeArt(media.art);

    if (!registered_) {
        bus.registerObject(kPath, this, QDBusConnection::ExportAdaptors);
        registered_ = bus.registerService(kService);
        return;  // a new player is read whole
    }
    changed(kPlayer, {
                         {QStringLiteral("PlaybackStatus"), player_->playbackStatus()},
                         {QStringLiteral("Metadata"), player_->metadata()},
                         {QStringLiteral("CanGoNext"), player_->canGoNext()},
                         {QStringLiteral("CanGoPrevious"), player_->canGoPrevious()},
                         {QStringLiteral("CanPlay"), player_->canPlay()},
                         {QStringLiteral("CanPause"), player_->canPause()},
                         {QStringLiteral("CanSeek"), player_->canSeek()},
                     });
    // Position isn't signalled as it ticks, only when it jumps.
    if (!newTrack && std::abs(std::int64_t(media.position) - expected) > 1500)
        emit player_->Seeked(qlonglong(media.position) * 1000);
}

void Mpris::changed(const QString& interface, const QVariantMap& properties) {
    QDBusMessage m = QDBusMessage::createSignal(kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                QStringLiteral("PropertiesChanged"));
    m << interface << properties << QStringList();
    QDBusConnection::sessionBus().send(m);
}

// A new name for each cover: players cache art by URL.
QString Mpris::writeArt(const std::string& jpeg) {
    if (!artUrl_.isEmpty())
        QFile::remove(QUrl(artUrl_).toLocalFile());
    if (jpeg.empty())
        return {};
    const QString path = runtime_ + QStringLiteral("/cover-%1.jpg").arg(++artSerial_);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(jpeg.data(), qint64(jpeg.size()));
    return QUrl::fromLocalFile(path).toString();
}

} // namespace atrium::phonelink
