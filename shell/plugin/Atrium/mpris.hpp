#pragma once
// Media players on the session bus (MPRIS 2): `Mpris.players`, and
// `Mpris.active`, the one playing (else the first). Position is in seconds
// and keeps moving while playing, though players only report jumps.

#include <QDBusVariant>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariant>

namespace atrium {

class MprisPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString service READ service CONSTANT)
    Q_PROPERTY(QString identity READ identity NOTIFY changed)
    Q_PROPERTY(QString desktopEntry READ desktopEntry NOTIFY changed)
    Q_PROPERTY(QString trackTitle READ trackTitle NOTIFY changed)
    Q_PROPERTY(QString trackArtist READ trackArtist NOTIFY changed)
    Q_PROPERTY(QString trackAlbum READ trackAlbum NOTIFY changed)
    Q_PROPERTY(QString trackArtUrl READ trackArtUrl NOTIFY changed)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY changed)
    Q_PROPERTY(double length READ length NOTIFY changed)
    Q_PROPERTY(bool lengthSupported READ lengthSupported NOTIFY changed)
    Q_PROPERTY(double position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(bool canTogglePlaying READ canTogglePlaying NOTIFY changed)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY changed)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY changed)
    Q_PROPERTY(bool canSeek READ canSeek NOTIFY changed)

public:
    MprisPlayer(const QString& service, QObject* parent);

    QString service() const { return service_; }
    QString identity() const { return identity_; }
    QString desktopEntry() const { return desktopEntry_; }
    QString trackTitle() const { return title_; }
    QString trackArtist() const { return artist_; }
    QString trackAlbum() const { return album_; }
    QString trackArtUrl() const { return artUrl_; }
    bool isPlaying() const { return status_ == "Playing"; }
    double length() const { return lengthUs_ / 1e6; }
    bool lengthSupported() const { return lengthUs_ > 0; }
    double position() const;
    void setPosition(double seconds);
    bool canTogglePlaying() const { return canControl_ && (canPlay_ || canPause_); }
    bool canGoNext() const { return canControl_ && canNext_; }
    bool canGoPrevious() const { return canControl_ && canPrevious_; }
    bool canSeek() const { return canControl_ && canSeek_; }

    Q_INVOKABLE void togglePlaying();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

signals:
    void changed();
    void positionChanged();

private slots:
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void seeked(qlonglong us);

private:
    void fetch(const QString& interface);
    void apply(const QVariantMap& props);
    void fetchPosition();
    void call(const QString& method, const QVariantList& args = {});

    QString service_;
    QString identity_, desktopEntry_, title_, artist_, album_, artUrl_, trackId_, status_;
    qlonglong lengthUs_ = 0;
    qlonglong positionUs_ = 0;  // at `since_`
    double rate_ = 1.0;
    QElapsedTimer since_;
    bool canControl_ = false, canPlay_ = false, canPause_ = false, canNext_ = false, canPrevious_ = false,
         canSeek_ = false;
    QTimer tick_;
};

class Mpris : public QObject {
    Q_OBJECT
    Q_PROPERTY(QList<QObject*> players READ players NOTIFY playersChanged)
    Q_PROPERTY(QObject* active READ active NOTIFY activeChanged)

public:
    static Mpris* instance();

    QList<QObject*> players() const;
    QObject* active() const;

signals:
    void playersChanged();
    void activeChanged();

private slots:
    void nameOwnerChanged(const QString& name, const QString& before, const QString& after);

private:
    Mpris();
    void add(const QString& service);
    void remove(const QString& service);

    QList<MprisPlayer*> players_;
};

} // namespace atrium
