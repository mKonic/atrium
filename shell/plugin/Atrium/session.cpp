#include "session.hpp"

#include "compositor.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>

namespace atrium {

namespace {

// org.freedesktop.login1.Manager.<method>(interactive): polkit may ask.
void logind(const char* method) {
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.login1", "/org/freedesktop/login1",
                                                       "org.freedesktop.login1.Manager", method);
    call << true;
    QDBusConnection::systemBus().asyncCall(call);
}

} // namespace

Session::Session(QObject* parent) : QObject(parent) {
    tick_.setInterval(1000);
    connect(&tick_, &QTimer::timeout, this, [this] {
        setSecondsLeft(secondsLeft_ - 1);
        if (secondsLeft_ <= 0)
            confirm();
    });
}

void Session::request(const QString& action) {
    if (action == "sleep") {
        run(action);
        return;
    }
    if (action != "restart" && action != "shutdown" && action != "logout")
        return;
    pending_ = action;
    emit pendingChanged();
    setSecondsLeft(kCountdown);
    tick_.start();
}

void Session::confirm() {
    const QString action = pending_;
    cancel();
    if (!action.isEmpty())
        run(action);
}

void Session::cancel() {
    tick_.stop();
    if (pending_.isEmpty())
        return;
    pending_.clear();
    emit pendingChanged();
}

void Session::run(const QString& action) {
    if (action == "sleep")
        logind("Suspend");
    else if (action == "restart")
        logind("Reboot");
    else if (action == "shutdown")
        logind("PowerOff");
    else if (action == "logout")
        Compositor::instance()->action("quit");
}

void Session::setSecondsLeft(int seconds) {
    if (seconds == secondsLeft_)
        return;
    secondsLeft_ = seconds;
    emit secondsLeftChanged();
}

} // namespace atrium
