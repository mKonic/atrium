#pragma once
// The phone's audio for the Phone page, from atrium-phonelink
// (org.atrium.PhoneLink on the session bus; daemon.hpp has the states):
// `PhoneLink.state` ("" when atrium-phonelink isn't running), `phone`, the
// pairing `code` while confirming, `error`, and the paired `phones`
// [{id, name}]. The calls go straight to the daemon.

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
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList phones READ phones NOTIFY changed)

public:
    explicit PhoneLink(QObject* parent = nullptr);

    QString state() const { return state_; }
    QString phone() const { return phone_; }
    QString code() const { return code_; }
    QString error() const { return error_; }
    QVariantList phones() const { return phones_; }

    Q_INVOKABLE void pair() { call(QStringLiteral("Pair")); }
    Q_INVOKABLE void cancelPairing() { call(QStringLiteral("CancelPairing")); }
    Q_INVOKABLE void accept() { call(QStringLiteral("Accept")); }
    Q_INVOKABLE void reject() { call(QStringLiteral("Reject")); }
    Q_INVOKABLE void forget(const QString& id) { call(QStringLiteral("Forget"), id); }

signals:
    void changed();

private slots:
    void load();

private:
    void call(const QString& method, const QString& arg = {});
    void clear();

    QString state_, phone_, code_, error_;
    QVariantList phones_;
    QDBusServiceWatcher* watcher_;
};

} // namespace atrium
