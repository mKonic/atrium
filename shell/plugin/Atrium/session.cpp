#include "session.hpp"

#include "compositor.hpp"

#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
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

QVariantList Session::waylandSessions() const {
    QStringList dirs = QString::fromLocal8Bit(qgetenv("XDG_DATA_DIRS")).split(':', Qt::SkipEmptyParts);
    if (dirs.isEmpty())
        dirs = {"/usr/local/share", "/usr/share"};
    QVariantList out;
    QStringList seen;
    for (const QString& dir : dirs) {
        const QDir sessions(dir + "/wayland-sessions");
        for (const QString& file : sessions.entryList({"*.desktop"}, QDir::Files, QDir::Name)) {
            if (seen.contains(file))
                continue;  // earlier directories win, as with any desktop file
            seen.append(file);
            QSettings entry(sessions.filePath(file), QSettings::IniFormat);
            entry.beginGroup("Desktop Entry");
            if (entry.value("Hidden").toBool() || entry.value("NoDisplay").toBool())
                continue;
            // Field codes (%U and the like) mean nothing here.
            QString exec = entry.value("Exec").toString();
            exec.remove(QRegularExpression("\\s*%[a-zA-Z]"));
            if (exec.isEmpty())
                continue;
            const QVariantMap s{{"name", entry.value("Name").toString()}, {"exec", exec.trimmed()},
                                {"argv", QProcess::splitCommand(exec.trimmed())}, {"id", file.chopped(8)}};
            if (file == "atrium.desktop")
                out.prepend(s);
            else
                out.append(s);
        }
    }
    return out;
}

namespace {

QString login_state_file() {
    QString base = QString::fromLocal8Bit(qgetenv("XDG_STATE_HOME"));
    if (base.isEmpty())
        base = QDir::homePath() + "/.local/state";
    return base + "/atrium/greeter.conf";
}

} // namespace

QVariantMap Session::lastLogin() const {
    QSettings state(login_state_file(), QSettings::IniFormat);
    return {{"user", state.value("Last/User").toString()}, {"session", state.value("Last/Session").toString()}};
}

void Session::rememberLogin(const QString& user, const QString& session) {
    QDir().mkpath(QFileInfo(login_state_file()).path());
    QSettings state(login_state_file(), QSettings::IniFormat);
    state.setValue("Last/User", user);
    state.setValue("Last/Session", session);
    state.sync();
}

void Session::setSecondsLeft(int seconds) {
    if (seconds == secondsLeft_)
        return;
    secondsLeft_ = seconds;
    emit secondsLeftChanged();
}

} // namespace atrium
