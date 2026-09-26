#pragma once
// The phone clipboard's status for the Bluetooth page, from atrium-clipsync
// (org.atrium.ClipSync on the session bus): `PhoneClipboard.state` ("off",
// "unavailable", "waiting", "connecting", "missing", "connected", or "" when
// atrium-clipsync isn't running) and `PhoneClipboard.phone`.

#include <QObject>
#include <QString>

class QDBusServiceWatcher;

namespace atrium {

class PhoneClipboard : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString phone READ phone NOTIFY changed)
    // Where the KernelSU module comes from.
    Q_PROPERTY(QString moduleUrl READ moduleUrl CONSTANT)

public:
    explicit PhoneClipboard(QObject* parent = nullptr);

    QString state() const { return state_; }
    QString phone() const { return phone_; }
    QString moduleUrl() const;

signals:
    void changed();

private slots:
    void statusChanged(const QString& state, const QString& phone);

private:
    void load();

    QString state_, phone_;
    QDBusServiceWatcher* watcher_;
};

} // namespace atrium
