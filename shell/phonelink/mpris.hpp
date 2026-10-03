#pragma once
// The phone's media session as an MPRIS player on the session bus
// (org.mpris.MediaPlayer2.atrium_phone), so atrium's media controls, the
// media keys and anything else that speaks MPRIS drive it. On the bus only
// while the phone has a session; commands go back through `command`.

#include "link_core.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace atrium::phonelink {

class Mpris;

class MprisRoot : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ no)
    Q_PROPERTY(bool CanRaise READ no)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none)

public:
    explicit MprisRoot(Mpris& m);
    bool no() const { return false; }
    QString identity() const;
    QStringList none() const { return {}; }

public slots:
    void Raise() {}
    void Quit() {}

private:
    Mpris& m_;
};

class MprisPlayer : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ one WRITE ignore)
    Q_PROPERTY(double MinimumRate READ one)
    Q_PROPERTY(double MaximumRate READ one)
    Q_PROPERTY(double Volume READ one WRITE ignore)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ yes)

public:
    explicit MprisPlayer(Mpris& m);
    QString playbackStatus() const;
    double one() const { return 1.0; }
    void ignore(double) {}
    QVariantMap metadata() const;
    qlonglong position() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const;
    bool canPause() const;
    bool canSeek() const;
    bool yes() const { return true; }

public slots:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offset);
    void SetPosition(const QDBusObjectPath& track, qlonglong position);
    void OpenUri(const QString&) {}

signals:
    void Seeked(qlonglong position);

private:
    Mpris& m_;
};

class Mpris : public QObject {
    Q_OBJECT

public:
    using Command = std::function<void(MediaCommand, std::uint32_t position)>;

    Mpris(QString runtimeDir, QObject* parent = nullptr);
    ~Mpris() override;

    // What `phone` plays now; Media{} (status None) takes it off the bus.
    void set(const QString& phone, const Media& media, Command command);
    void clear() { set({}, {}, {}); }

private:
    friend class MprisRoot;
    friend class MprisPlayer;

    std::uint32_t positionMs() const;
    void send(MediaCommand c, std::uint32_t position = 0);
    void changed(const QString& interface, const QVariantMap& properties);
    QString writeArt(const std::string& jpeg);

    QString runtime_;
    QString phone_;
    Media media_;
    Command command_;
    QElapsedTimer since_;  // since media_.position was true
    QString artUrl_;
    int track_ = 0, artSerial_ = 0;
    bool registered_ = false;
    MprisPlayer* player_;
};

} // namespace atrium::phonelink
