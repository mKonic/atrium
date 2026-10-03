#pragma once
// Phone audio for the Phone page, from atrium-phonelink
// (org.atrium.PhoneLink on the session bus; daemon.hpp has the details):
// `PhoneLink.state` ("" when atrium-phonelink isn't running, else "off",
// "on" or "confirm"), the pairing `phone` and `code` while confirming, the
// paired `phones` [{id, name, auto, state, error}] and the `nearby` ones
// [{id, name, ready, state, error}]. The calls go straight to the daemon.

#include <QObject>
#include <QString>
#include <QVariantList>

class QDBusServiceWatcher;

namespace atrium {

class PhoneLink : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString phone READ phone NOTIFY changed)
    Q_PROPERTY(QString code READ code NOTIFY changed)
    Q_PROPERTY(QVariantList phones READ phones NOTIFY changed)
    Q_PROPERTY(QVariantList nearby READ nearby NOTIFY changed)

public:
    explicit PhoneLink(QObject* parent = nullptr);

    QString state() const { return state_; }
    QString phone() const { return phone_; }
    QString code() const { return code_; }
    QVariantList phones() const { return phones_; }
    QVariantList nearby() const { return nearby_; }

    Q_INVOKABLE void pairWith(const QString& id) { call(QStringLiteral("PairWith"), {id}); }
    Q_INVOKABLE void cancelPairing() { call(QStringLiteral("CancelPairing")); }
    Q_INVOKABLE void accept() { call(QStringLiteral("Accept")); }
    Q_INVOKABLE void reject() { call(QStringLiteral("Reject")); }
    Q_INVOKABLE void connectPhone(const QString& id) { call(QStringLiteral("Connect"), {id}); }
    Q_INVOKABLE void disconnectPhone(const QString& id) { call(QStringLiteral("Disconnect"), {id}); }
    Q_INVOKABLE void setAutoConnect(const QString& id, bool on) { call(QStringLiteral("SetAutoConnect"), {id, on}); }
    Q_INVOKABLE void forget(const QString& id) { call(QStringLiteral("Forget"), {id}); }

signals:
    void changed();

private slots:
    void load();

private:
    void call(const QString& method, const QVariantList& args = {});
    void clear();

    QString state_, phone_, code_;
    QVariantList phones_, nearby_;
    QDBusServiceWatcher* watcher_;
};

} // namespace atrium
