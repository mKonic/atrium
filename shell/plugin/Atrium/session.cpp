#include "session.hpp"

#include "compositor.hpp"

#include "Shell/desktop_entries.hpp"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QFile>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    if (action == "logout")
        Compositor::instance()->action("quit");
    // Apps asked to quit first; the compositor restarts or shuts down once they have.
    else if (action == "restart" || action == "shutdown")
        Compositor::instance()->action("quit", action);
    else if (!action.isEmpty())
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

void Session::reopenApps(QObject* entries) {
    QString base = QString::fromLocal8Bit(qgetenv("XDG_STATE_HOME"));
    if (base.isEmpty())
        base = QDir::homePath() + "/.local/state";
    QFile file(base + "/atrium/reopen.json");
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray apps = QJsonDocument::fromJson(file.readAll()).object().value("apps").toArray();
    file.close();
    file.remove();  // once: a crash during the next session doesn't replay this one
    // A moment in: the windows the compositor knows of are in, and login
    // items have begun opening theirs.
    QPointer<QObject> source(entries);
    QTimer::singleShot(2000, this, [apps, source] {
        QStringList open;
        for (const QVariant& w : Compositor::instance()->windows())
            open << w.toMap().value("app_id").toString();
        auto* index = qobject_cast<shell::DesktopEntries*>(source.data());
        if (!index)
            index = shell::DesktopEntries::instance();
        for (const QJsonValue& v : apps) {
            const QString id = v.toString();
            if (id.isEmpty() || open.contains(id))
                continue;  // already back (a login item)
            if (QObject* entry = index->heuristicLookup(id))
                QMetaObject::invokeMethod(entry, "execute");
        }
    });
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
    // The greeter user's own directory (greeter-tmpfiles.conf, installed with
    // greetd's config): its home is often / and not its to write.
    const QFileInfo shared("/var/lib/atrium-greeter");
    if (shared.isDir() && shared.isWritable())
        return shared.filePath() + "/greeter.conf";
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

bool Session::canSwitchUser() const {
    if (QFileInfo::exists(QStringLiteral("/run/atrium-login/control.sock")))
        return true;
    const QString seat = qEnvironmentVariable("XDG_SEAT_PATH");
    if (seat.isEmpty())
        return false;
    QDBusInterface dm(QStringLiteral("org.freedesktop.DisplayManager"), seat,
                      QStringLiteral("org.freedesktop.DisplayManager.Seat"), QDBusConnection::systemBus());
    return dm.isValid() && dm.property("CanSwitch").toBool();
}

QString Session::sessionFor(const QString& user) const {
    if (user.isEmpty())
        return {};
    QSettings state(login_state_file(), QSettings::IniFormat);
    // Before there was one a person, only the last login's.
    const QString fallback = state.value("Last/User").toString() == user ? state.value("Last/Session").toString() : QString();
    return state.value("Sessions/" + user, fallback).toString();
}

void Session::rememberLogin(const QString& user, const QString& session) {
    QDir().mkpath(QFileInfo(login_state_file()).path());
    QSettings state(login_state_file(), QSettings::IniFormat);
    state.setValue("Last/User", user);
    state.setValue("Last/Session", session);
    state.setValue("Sessions/" + user, session);
    state.sync();
}

void Session::setSecondsLeft(int seconds) {
    if (seconds == secondsLeft_)
        return;
    secondsLeft_ = seconds;
    emit secondsLeftChanged();
}

} // namespace atrium
