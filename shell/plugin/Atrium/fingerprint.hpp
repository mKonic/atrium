#pragma once
// Fingerprints for the lock screen: fprintd over the system bus, as hyprlock
// does, beside the password field. It listens while the session is locked
// and the user has fingers enrolled; a match unlocks.

#include "fingerprint_core.hpp"

#include <QDBusObjectPath>
#include <QObject>
#include <QString>

namespace atrium {

class Fingerprint : public QObject {
    Q_OBJECT

public:
    explicit Fingerprint(QObject* parent = nullptr);
    ~Fingerprint() override;

    bool listening() const { return listening_; }
    // Claims the default reader and starts listening (if anything's enrolled).
    void start();
    void stop();

signals:
    void listeningChanged();
    void matched();
    void message(const QString& text);

private slots:
    void onVerifyStatus(const QString& result, bool done);
    void onPrepareForSleep(bool sleeping);

private:
    bool call(const char* method, const QVariantList& args = {}, QString* error = nullptr);
    void setListening(bool on);

    QString user_;
    QString device_;  // its object path, once claimed
    bool listening_ = false;
    int misses_ = 0;
};

} // namespace atrium
