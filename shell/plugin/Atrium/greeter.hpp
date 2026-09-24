#pragma once
// Logging in through greetd, for the login screen: `Greeter.login(user,
// password, session)` runs greetd's exchange (create the session, answer
// its questions with the password, start the chosen desktop) and quits the
// greeter when the desktop starts. The protocol is JSON over the socket in
// GREETD_SOCK, each message preceded by its length.

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QStringList>

namespace atrium {

class Greeter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)  // what went wrong, or ""

public:
    explicit Greeter(QObject* parent = nullptr);

    bool available() const { return !path_.isEmpty(); }
    bool busy() const { return busy_; }
    QString message() const { return message_; }

    // `session` is one of Session.waylandSessions(): { id, argv, ... }.
    Q_INVOKABLE void login(const QString& user, const QString& password, const QVariantMap& session);
    Q_INVOKABLE void clearMessage();

signals:
    void changed();
    // The password was wrong (the screen shakes).
    void failed();

private:
    void send(const QJsonObject& request);
    void read();
    void reply(const QJsonObject& r);
    void fail(const QString& message, bool wrong);

    QString path_;
    QLocalSocket socket_;
    QByteArray buffer_;
    // Where greetd's exchange is: its replies mean different things in each.
    enum class Step { Idle, Authenticating, Starting };
    Step step_ = Step::Idle;
    int cancels_ = 0;  // cancel_session replies still to come
    bool busy_ = false;
    QString message_, user_, password_, sessionId_;
    QStringList argv_;
};

} // namespace atrium
