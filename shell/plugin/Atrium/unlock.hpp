#pragma once
// The lock screen's password check: PAM's atrium-lock service for the user
// running it (pam_unix checks through its setuid helper), off the UI thread.
// Right, it unlocks the session (the atrium-lock shell integration's lock)
// and the lock screen quits.

#include <QObject>
#include <QString>

namespace atrium {

class Unlock : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)  // what went wrong, or ""
    Q_PROPERTY(bool locked READ locked NOTIFY changed)       // the compositor has locked

public:
    explicit Unlock(QObject* parent = nullptr);

    bool busy() const { return busy_; }
    QString message() const { return message_; }
    bool locked() const;

    Q_INVOKABLE void tryPassword(const QString& password);
    Q_INVOKABLE void clearMessage();

    // PAM's verdict on `password` for `user` (blocking).
    static bool check(const QString& service, const QString& user, const QString& password, QString* error);

signals:
    void changed();
    void failed();

private:
    void finish(bool ok, const QString& error);

    bool busy_ = false;
    QString message_;
};

} // namespace atrium
