#include "fingerprint.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QTimer>

#include <pwd.h>
#include <unistd.h>

namespace atrium {

namespace {

constexpr const char* kService = "net.reactivated.Fprint";
constexpr const char* kDevice = "net.reactivated.Fprint.Device";

} // namespace

Fingerprint::Fingerprint(QObject* parent) : QObject(parent) {
    if (const passwd* pw = getpwuid(getuid()))
        user_ = QString::fromLocal8Bit(pw->pw_name);
    // A reader can lose its state over sleep; listen afresh after.
    QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
                                         "org.freedesktop.login1.Manager", "PrepareForSleep", this,
                                         SLOT(onPrepareForSleep(bool)));
}

Fingerprint::~Fingerprint() {
    stop();
}

bool Fingerprint::call(const char* method, const QVariantList& args, QString* error) {
    QDBusMessage m = QDBusMessage::createMethodCall(kService, device_, kDevice, method);
    m.setArguments(args);
    const QDBusMessage r = QDBusConnection::systemBus().call(m, QDBus::Block, 3000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        if (error)
            *error = r.errorName();
        return false;
    }
    return true;
}

void Fingerprint::setListening(bool on) {
    if (on == listening_)
        return;
    listening_ = on;
    emit listeningChanged();
}

void Fingerprint::start() {
    if (!device_.isEmpty() || user_.isEmpty())
        return;
    QDBusMessage get = QDBusMessage::createMethodCall(kService, "/net/reactivated/Fprint/Manager",
                                                      "net.reactivated.Fprint.Manager", "GetDefaultDevice");
    const QDBusReply<QDBusObjectPath> reply = QDBusConnection::systemBus().call(get, QDBus::Block, 3000);
    if (!reply.isValid()) {
        qInfo("fingerprint: none: %s", qPrintable(reply.error().name()));  // no fprintd, or no reader
        return;
    }
    device_ = reply.value().path();
    QDBusMessage list = QDBusMessage::createMethodCall(kService, device_, kDevice, "ListEnrolledFingers");
    list << user_;
    const QDBusReply<QStringList> fingers = QDBusConnection::systemBus().call(list, QDBus::Block, 3000);
    if (!fingers.isValid() || fingers.value().isEmpty()) {
        qInfo("fingerprint: nothing enrolled for %s%s%s", qPrintable(user_), fingers.isValid() ? "" : ": ",
              qPrintable(fingers.error().name()));
        device_.clear();
        return;  // the password only
    }
    QString error;
    if (!call("Claim", {user_}, &error)) {
        qWarning("fingerprint: can't claim the reader: %s", qPrintable(error));
        device_.clear();
        return;
    }
    QDBusConnection::systemBus().connect(kService, device_, kDevice, "VerifyStatus", this,
                                         SLOT(onVerifyStatus(QString, bool)));
    misses_ = 0;
    if (!call("VerifyStart", {QStringLiteral("any")}, &error)) {
        qWarning("fingerprint: can't start: %s", qPrintable(error));
        stop();
        return;
    }
    qInfo("fingerprint: listening on %s", qPrintable(device_));
    setListening(true);
}

void Fingerprint::stop() {
    if (device_.isEmpty())
        return;
    QDBusConnection::systemBus().disconnect(kService, device_, kDevice, "VerifyStatus", this,
                                            SLOT(onVerifyStatus(QString, bool)));
    if (listening_)
        call("VerifyStop");
    call("Release");
    device_.clear();
    setListening(false);
}

void Fingerprint::onVerifyStatus(const QString& result, bool done) {
    const FingerprintStep s = fingerprint_step(result.toStdString(), done, misses_);
    misses_ = s.misses;
    if (!s.message.empty())
        emit message(QString::fromStdString(s.message));
    switch (s.action) {
    case FingerprintStep::Action::Unlock:
        stop();
        emit matched();
        break;
    case FingerprintStep::Action::Continue:
        break;
    case FingerprintStep::Action::Restart:
        // Not from inside fprintd's own signal: a moment later.
        QTimer::singleShot(0, this, [this] {
            if (device_.isEmpty())
                return;
            call("VerifyStop");
            if (!call("VerifyStart", {QStringLiteral("any")}))
                stop();
        });
        break;
    case FingerprintStep::Action::Stop:
        stop();
        break;
    }
}

void Fingerprint::onPrepareForSleep(bool sleeping) {
    if (sleeping) {
        stop();
    } else {
        QTimer::singleShot(1000, this, [this] { start(); });
    }
}

} // namespace atrium
