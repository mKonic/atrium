#include "greeter.hpp"

#include "session.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstring>

namespace atrium {

Greeter::Greeter(QObject* parent) : QObject(parent), path_(qEnvironmentVariable("GREETD_SOCK")) {
    connect(&socket_, &QLocalSocket::readyRead, this, &Greeter::read);
    connect(&socket_, &QLocalSocket::errorOccurred, this, [this] {
        if (busy_)
            fail(QStringLiteral("The login service stopped answering."), false);
    });
}

void Greeter::clearMessage() {
    if (message_.isEmpty())
        return;
    message_.clear();
    emit changed();
}

void Greeter::login(const QString& user, const QString& password, const QVariantMap& session) {
    if (busy_ || user.isEmpty())
        return;
    if (!available()) {
        fail(QStringLiteral("The login service (greetd) isn't running."), false);
        return;
    }
    user_ = user;
    password_ = password;
    sessionId_ = session.value("id").toString();
    argv_ = session.value("argv").toStringList();
    if (argv_.isEmpty())
        argv_ = {QStringLiteral("atrium")};
    busy_ = true;
    step_ = Step::Authenticating;
    message_.clear();
    emit changed();
    if (socket_.state() != QLocalSocket::ConnectedState) {
        socket_.connectToServer(path_);
        if (!socket_.waitForConnected(2000)) {
            fail(QStringLiteral("The login service (greetd) isn't running."), false);
            return;
        }
    }
    send({{"type", "create_session"}, {"username", user}});
}

void Greeter::send(const QJsonObject& request) {
    const QByteArray body = QJsonDocument(request).toJson(QJsonDocument::Compact);
    const quint32 len = quint32(body.size());
    char header[4];
    std::memcpy(header, &len, 4);  // native byte order, as greetd reads it
    socket_.write(header, 4);
    socket_.write(body);
    socket_.flush();
}

void Greeter::read() {
    buffer_ += socket_.readAll();
    while (buffer_.size() >= 4) {
        quint32 len;
        std::memcpy(&len, buffer_.constData(), 4);
        if (buffer_.size() < qsizetype(4 + len))
            return;
        const QJsonDocument doc = QJsonDocument::fromJson(buffer_.mid(4, len));
        buffer_.remove(0, 4 + len);
        reply(doc.object());
    }
}

void Greeter::reply(const QJsonObject& r) {
    const QString type = r.value("type").toString();
    if (cancels_ > 0) {
        --cancels_;  // a cancel's answer, whatever came after it
        return;
    }
    if (step_ == Step::Idle)
        return;
    if (type == "auth_message") {
        const QString kind = r.value("auth_message_type").toString();
        if (kind == "secret" || kind == "visible") {
            send({{"type", "post_auth_message_response"}, {"response", password_}});
        } else {
            // info and error: shown, and greetd waits for an empty answer.
            if (kind == "error") {
                message_ = r.value("auth_message").toString();
                emit changed();
            }
            send({{"type", "post_auth_message_response"}});
        }
    } else if (type == "success") {
        if (step_ == Step::Authenticating) {
            // Authenticated: start the desktop.
            step_ = Step::Starting;
            password_.fill(QChar(0));
            password_.clear();
            send({{"type", "start_session"}, {"cmd", QJsonArray::fromStringList(argv_)}, {"env", QJsonArray()}});
            return;
        }
        // The desktop starts once this greeter is gone.
        Session::rememberLogin(user_, sessionId_);
        QCoreApplication::exit(0);
    } else if (type == "error") {
        const bool wrong = r.value("error_type").toString() == "auth_error";
        fail(wrong ? QStringLiteral("Incorrect password.") : r.value("description").toString(), wrong);
    }
}

void Greeter::fail(const QString& message, bool wrong) {
    // A half-made session would refuse the next create_session.
    if (socket_.state() == QLocalSocket::ConnectedState && step_ != Step::Idle) {
        send({{"type", "cancel_session"}});
        ++cancels_;
    }
    step_ = Step::Idle;
    password_.fill(QChar(0));
    password_.clear();
    busy_ = false;
    message_ = message;
    emit changed();
    if (wrong)
        emit failed();
}

} // namespace atrium
