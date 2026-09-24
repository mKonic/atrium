#pragma once
// Sleep, restart, shut down and log out, as the session menu offers them.
// Restart, shut down and log out ask first and go ahead by themselves after
// a minute, as macOS does; the power ones go to logind.

#include <QObject>
#include <QTimer>
#include <QVariant>

namespace atrium {

class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString pending READ pending NOTIFY pendingChanged)  // "", "restart", "shutdown", "logout"
    Q_PROPERTY(int secondsLeft READ secondsLeft NOTIFY secondsLeftChanged)

public:
    static constexpr int kCountdown = 60;

    explicit Session(QObject* parent = nullptr);

    QString pending() const { return pending_; }
    int secondsLeft() const { return secondsLeft_; }

    // "sleep" happens now; the others wait for confirm() or the countdown.
    Q_INVOKABLE void request(const QString& action);
    Q_INVOKABLE void confirm();
    Q_INVOKABLE void cancel();
    // At once, without asking (the login screen's buttons).
    Q_INVOKABLE void now(const QString& action) { run(action); }
    // The desktops installed (wayland-sessions): [{id, name, exec, argv}],
    // atrium first.
    Q_INVOKABLE QVariantList waylandSessions() const;
    // The login screen's memory, as SDDM's [Last] in state.conf: who logged
    // in last and into which desktop (a session id), kept in the greeter
    // user's state directory.
    Q_INVOKABLE QVariantMap lastLogin() const;
    Q_INVOKABLE void rememberLogin(const QString& user, const QString& session);

signals:
    void pendingChanged();
    void secondsLeftChanged();

private:
    void run(const QString& action);
    void setSecondsLeft(int seconds);

    QString pending_;
    int secondsLeft_ = 0;
    QTimer tick_;
};

} // namespace atrium
