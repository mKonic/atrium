#pragma once
// Sleep, restart, shut down and log out, as the session menu offers them.
// Restart, shut down and log out ask first and go ahead by themselves after
// a minute, as macOS does; the power ones go to logind.

#include <QObject>
#include <QTimer>

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
