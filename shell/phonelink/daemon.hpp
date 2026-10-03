#pragma once
// atrium-phonelink's whole job: while phone.audio is on, find paired phones
// on the network (their module announces _atrium-link._tcp), connect, and
// play what they play (Playback). Pairing is the user's: Pair() opens a
// window in which unpaired phones are tried too, both ends show a code,
// both users accept.
//
// For the Settings app it is org.atrium.PhoneLink on the session bus:
// State, Phone (the phone it is about), Code (while confirming), Phones and
// PhoneIds (the paired ones), with StatusChanged on every change. State is
// "off", "searching" (no paired phone seen), "pairing" (waiting for a phone
// that is pairing too), "confirm" (Code shown: Accept or Reject),
// "connecting", "connected" (the phone isn't sending sound), "streaming" or
// "failed" (Error says why).
//
// The phone's media session shows up as an MPRIS player (mpris.hpp).

#include "link_core.hpp"
#include "mpris.hpp"
#include "playback.hpp"

#include <QHostAddress>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>

#include <memory>

namespace atrium::phonelink {

class Daemon;

class Status : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.atrium.PhoneLink1")
    Q_PROPERTY(QString State MEMBER state)
    Q_PROPERTY(QString Phone MEMBER phone)
    Q_PROPERTY(QString Code MEMBER code)
    Q_PROPERTY(QString Error MEMBER error)
    Q_PROPERTY(QStringList Phones MEMBER phones)
    Q_PROPERTY(QStringList PhoneIds MEMBER phoneIds)

public:
    explicit Status(Daemon& d);

    QString state = QStringLiteral("off"), phone, code, error;
    QStringList phones, phoneIds;

public slots:
    void Pair();
    void CancelPairing();
    void Accept();
    void Reject();
    void Forget(const QString& id);

signals:
    void StatusChanged();

private:
    Daemon& d_;
};

// A phone seen on the network.
struct Service {
    QString name;
    QHostAddress address;
    quint16 port = 0;
    // To resolve it again: avahi says nothing when only the port changed.
    AvahiIfIndex iface = AVAHI_IF_UNSPEC;
    AvahiProtocol protocol = AVAHI_PROTO_UNSPEC;
    QByteArray domain;
};

class Daemon : public QObject {
    Q_OBJECT

public:
    explicit Daemon(QObject* parent = nullptr);
    ~Daemon() override;

    void pair();
    void cancelPairing();
    void answer(bool accept);
    void forget(const QString& id);

private:
    struct Conn {
        QString id;  // hex, from mDNS until HELLO says
        std::unique_ptr<QTcpSocket> socket;
        std::unique_ptr<Link> link;
        std::unique_ptr<Playback> playback;
        std::uint32_t nackCounter = 0;
        bool streaming = false, closed = false;
        QString name, failed;
        Media media;
    };

    void setEnabled(bool on);
    void startAvahi();
    static void clientEvent(AvahiClient* c, AvahiClientState state, void* self);
    static void browseEvent(AvahiServiceBrowser*, AvahiIfIndex, AvahiProtocol, AvahiBrowserEvent, const char* name,
                            const char* type, const char* domain, AvahiLookupResultFlags, void* self);
    static void resolveEvent(AvahiServiceResolver*, AvahiIfIndex, AvahiProtocol, AvahiResolverEvent, const char* name,
                             const char* type, const char* domain, const char* host, const AvahiAddress* a,
                             uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* self);

    void reconcile();
    void resolve(const Service& s);
    void connectTo(const QString& id, const Service& s);
    void apply(Conn* c, std::vector<Event> events);
    void message(Conn* c, Type type, const std::string& body);
    void drop(Conn* c, const QString& why);
    void updateMedia();
    void loadPaired();
    void savePaired();
    bool pairingOpen() const;
    void updateStatus();

    QString dir_;
    Identity me_;
    struct Paired {
        std::string key;
        QString name;
    };
    QMap<QString, Paired> paired_;      // by hex id
    QMap<QString, Service> services_;   // by hex id
    QMap<QString, QString> serviceIds_;  // mDNS name -> hex id
    QMap<QString, Conn*> conns_;        // by hex id
    QMap<QString, qint64> nextTry_;     // by hex id, ms since epoch
    Conn* confirming_ = nullptr;
    QString code_;
    QString lastError_;
    qint64 pairUntil_ = 0;
    bool enabled_ = false;
    std::uint32_t target_;
    QTimer retry_, pairTimer_;
    Status* status_;
    Mpris* mpris_;
    AvahiClient* avahi_ = nullptr;
    AvahiServiceBrowser* browser_ = nullptr;
};

} // namespace atrium::phonelink
