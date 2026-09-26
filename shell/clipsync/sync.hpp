#pragma once
// atrium-clipsync's whole job: while bluetooth.phone_clipboard is on, every
// paired phone that connects gets an RFCOMM connection to its clipboard
// module (rfcomm.hpp), and a Session per connection moves clips between it
// and the Wayland clipboard.
//
// For the Settings app it is org.atrium.ClipSync on the session bus:
// State and Phone (the connected phone's name), with StatusChanged on every
// change. State is "off", "unavailable" (no Bluetooth, or no clipboard),
// "waiting" (no phone connected), "connecting", "missing" (a phone is
// connected but its module doesn't answer) or "connected".

#include "clipsync_core.hpp"
#include "history.hpp"
#include "wayland_clipboard.hpp"

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantMap>

#include <memory>

namespace atrium {
class BluetoothDevice;
}

namespace atrium::clipsync {

class Sync;

// One phone's connection.
class Link : public QObject {
    Q_OBJECT

public:
    Link(Sync& sync, const QString& device, const QString& name, std::vector<Clip> recent, int fd);
    ~Link() override;

    const QString& device() const { return device_; }
    Session& session() { return session_; }
    void apply(const std::vector<Action>& actions);

signals:
    void closed();

private:
    void readable();
    void pump();
    void close();

    Sync& sync_;
    QString device_;
    int fd_;
    Session session_;
    std::string out_;
    std::unique_ptr<QSocketNotifier> read_, write_;
    bool closed_ = false;
};

// The status the Settings app shows.
class Status : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.atrium.ClipSync1")
    Q_PROPERTY(QString State READ state)
    Q_PROPERTY(QString Phone READ phone)

public:
    explicit Status(QObject* parent) : QObject(parent) {}
    QString state() const { return state_; }
    QString phone() const { return phone_; }
    void set(const QString& state, const QString& phone);

signals:
    void StatusChanged(const QString& state, const QString& phone);

private:
    QString state_ = QStringLiteral("off"), phone_;
};

class Sync : public QObject {
    Q_OBJECT

public:
    explicit Sync(QObject* parent = nullptr);
    ~Sync() override;

    bool ok() const { return clipboard_.ok(); }

    // From a Link.
    void setClipboard(Link* from, const Clip& c);
    void addHistory(const Clip& c);
    void merged(bool tookClipboard);


private:
    void setEnabled(bool on);
    void connected(const QString& device, int fd);
    void devicesChanged();
    void tryConnect(BluetoothDevice* d);
    void schedule(BluetoothDevice* d, int ms);
    void copied(const Clip& c);
    void updateStatus();
    bool isPhone(const BluetoothDevice* d) const;

    WaylandClipboard clipboard_;
    std::unique_ptr<RecentClips> history_;  // made when first turned on
    Status* status_;
    bool enabled_ = false;
    QMap<QString, Link*> links_;              // by device path
    QMap<QString, QTimer*> retries_;          // by device path
    QMap<QString, int> attempts_;
    QMap<QString, bool> connecting_;
    bool addedHistory_ = false;
};

} // namespace atrium::clipsync
