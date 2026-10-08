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
    // Apps that haven't quit for a confirmed log out, restart or shut down:
    // what it is waiting to do, their names, and the seconds until it goes
    // ahead anyway ("" and none when it isn't waiting).
    Q_PROPERTY(QString waitingFor READ waitingFor NOTIFY waitingChanged)
    Q_PROPERTY(QStringList holdouts READ holdouts NOTIFY waitingChanged)
    Q_PROPERTY(int waitingSeconds READ waitingSeconds NOTIFY waitingChanged)

public:
    static constexpr int kCountdown = 60;

    explicit Session(QObject* parent = nullptr);

    QString pending() const { return pending_; }
    int secondsLeft() const { return secondsLeft_; }
    QString waitingFor() const { return waitingFor_; }
    QStringList holdouts() const { return holdouts_; }
    int waitingSeconds() const { return waitingSeconds_; }
    // The apps still open: go ahead without them, or call it off.
    Q_INVOKABLE void goAnyway();
    Q_INVOKABLE void stopWaiting();

    // "sleep" happens now; the others wait for confirm() or the countdown.
    Q_INVOKABLE void request(const QString& action);
    Q_INVOKABLE void confirm();
    Q_INVOKABLE void cancel();
    // The apps open at the last logout (reopen.json, from the compositor),
    // opened again, once.
    Q_INVOKABLE void reopenApps(QObject* entries = nullptr);
    // At once, without asking (the login screen's buttons).
    Q_INVOKABLE void now(const QString& action) { run(action); }
    // The desktops installed (wayland-sessions): [{id, name, exec, argv}],
    // atrium first.
    Q_INVOKABLE QVariantList waylandSessions() const;
    // The login screen's memory, as SDDM's [Last] in state.conf: who logged
    // in last and into which desktop (a session id), kept in the greeter
    // user's state directory.
    Q_INVOKABLE QVariantMap lastLogin() const;
    // The desktop `user` logged into last ("" when they never have).
    Q_INVOKABLE QString sessionFor(const QString& user) const;
    // A login manager can show a greeter beside this session (atrium-login,
    // or a display manager's seat that can switch): Switch User works.
    Q_INVOKABLE bool canSwitchUser() const;
    Q_INVOKABLE static void rememberLogin(const QString& user, const QString& session);

signals:
    void pendingChanged();
    void secondsLeftChanged();
    void waitingChanged();

private:
    void run(const QString& action);
    void setSecondsLeft(int seconds);

    QString pending_;
    int secondsLeft_ = 0;
    QTimer tick_;
    QString waitingFor_;
    QStringList holdouts_;
    int waitingSeconds_ = 0;
};

} // namespace atrium
