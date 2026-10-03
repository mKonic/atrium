#pragma once
// atrium-phonelink's whole job: while phone.audio is on, find phones on the
// network (their module announces _atrium-link._tcp), connect to the paired
// ones that should be, and play what they play (Playback). Like Bluetooth:
// each paired phone connects by itself unless told not to (its "connect
// automatically" switch), Connect and Disconnect hold for this session, and
// a nearby phone is paired by picking it while the phone pairs too; both
// ends show a code, both users accept.
//
// For the Settings app it is org.atrium.PhoneLink on the session bus, with
// StatusChanged on every change:
// - State: "off", "on", or "confirm" while a pairing code is shown: Phone
//   names the phone, Code is the code (Accept or Reject).
// - Phones, the paired ones: [{id, name, auto, state, error}], state one of
//   "streaming", "connected" (the phone isn't sending sound), "connecting",
//   "failed" (its sound can't come here: error says why), "disconnected"
//   (by hand, or not connecting by itself), "away" (not on the network) or
//   "forgot" (the phone no longer has this PC paired).
// - Nearby, unpaired phones on the network: [{id, name, ready, state,
//   error}], ready while the phone is pairing, state "pairing" while
//   PairWith is under way, error why the last try didn't pair.
//
// The phone's media session shows up as an MPRIS player (mpris.hpp).

#include "link_core.hpp"
#include "mpris.hpp"
#include "playback.hpp"

#include <QHostAddress>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantMap>

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
    Q_PROPERTY(QList<QVariantMap> Phones MEMBER phones)
    Q_PROPERTY(QList<QVariantMap> Nearby MEMBER nearby)

public:
    explicit Status(Daemon& d);

    QString state = QStringLiteral("off"), phone, code;
    QList<QVariantMap> phones, nearby;

public slots:
    void PairWith(const QString& id);
    void CancelPairing();
    void Accept();
    void Reject();
    void Connect(const QString& id);
    void Disconnect(const QString& id);
    void SetAutoConnect(const QString& id, bool on);
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
    // From its TXT record, kept up to date (see link_core.hpp).
    bool pairing = false;
    QString call;
    AvahiRecordBrowser* txt = nullptr;
};

class Daemon : public QObject {
    Q_OBJECT

public:
    explicit Daemon(QObject* parent = nullptr);
    ~Daemon() override;

    void pairWith(const QString& id);
    void cancelPairing();
    void answer(bool accept);
    void connectPhone(const QString& id);
    void disconnectPhone(const QString& id);
    void setAutoConnect(const QString& id, bool on);
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

    static void txtEvent(AvahiRecordBrowser*, AvahiIfIndex, AvahiProtocol, AvahiBrowserEvent, const char* name,
                         uint16_t clazz, uint16_t type, const void* rdata, size_t size, AvahiLookupResultFlags,
                         void* self);
    static void readTxt(Service& s, AvahiStringList* txt);
    void forgetServices();

    bool wants(const QString& id) const;
    void reconcile();
    void resolve(const Service& s);
    void connectTo(const QString& id, const Service& s);
    void apply(Conn* c, std::vector<Event> events);
    void message(Conn* c, Type type, const std::string& body);
    void drop(Conn* c, const QString& why);
    void updateMedia();
    void loadPaired();
    void savePaired();
    void updateStatus();

    QString dir_;
    Identity me_;
    struct Paired {
        std::string key;
        QString name;
        bool autoConnect = true;
    };
    QMap<QString, Paired> paired_;      // by hex id
    QMap<QString, Service> services_;   // by hex id
    QMap<QString, QString> serviceIds_;  // mDNS name -> hex id
    QMap<QString, Conn*> conns_;        // by hex id
    QMap<QString, qint64> nextTry_;     // by hex id, ms since epoch
    QMap<QString, bool> manual_;        // by hex id: Connect or Disconnect, this session, over autoConnect
    QSet<QString> forgotBy_;            // phones that turned this PC away as unknown
    QString pairingWith_;               // the nearby phone PairWith is pairing
    QString pairFailed_, pairError_;    // the last one that didn't pair, and why
    Conn* confirming_ = nullptr;
    QString code_;
    bool enabled_ = false;
    std::uint32_t target_;
    QTimer retry_;
    Status* status_;
    Mpris* mpris_;
    AvahiClient* avahi_ = nullptr;
    AvahiServiceBrowser* browser_ = nullptr;
};

} // namespace atrium::phonelink
