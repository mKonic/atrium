#include "portal_secret.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusVariant>

#include <sys/random.h>
#include <unistd.h>

#include <cerrno>
#include <functional>
#include <memory>

namespace atrium {

namespace {

const QString kSecrets = QStringLiteral("org.freedesktop.secrets");
const QString kBase = QStringLiteral("/org/freedesktop/secrets");
const QString kService = QStringLiteral("org.freedesktop.Secret.Service");

struct Secret {
    QDBusObjectPath session;
    QByteArray parameters, value;
    QString content_type;
};

QDBusArgument& operator<<(QDBusArgument& a, const Secret& s) {
    a.beginStructure();
    a << s.session << s.parameters << s.value << s.content_type;
    a.endStructure();
    return a;
}

const QDBusArgument& operator>>(const QDBusArgument& a, Secret& s) {
    a.beginStructure();
    a >> s.session >> s.parameters >> s.value >> s.content_type;
    a.endStructure();
    return a;
}

using Paths = QList<QDBusObjectPath>;
using StringMap = QMap<QString, QString>;
using SecretMap = QMap<QDBusObjectPath, Secret>;

template <class T>
T cast(const QVariant& v) {
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<T>(v.value<QDBusArgument>());
    return v.value<T>();
}

bool write_all(int fd, const QByteArray& data) {
    qsizetype off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.constData() + off, size_t(data.size() - off));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        off += n;
    }
    return true;
}

// `objects` open, then `then(true)`; through the keyring's prompt if it
// asks; `then(false)` if dismissed or it failed.
void unlocked(const Paths& objects, std::function<void(bool)> then) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage reply = QDBusInterface(kSecrets, kBase, kService, bus).call("Unlock", QVariant::fromValue(objects));
    if (reply.type() != QDBusMessage::ReplyMessage)
        return then(false);
    const QDBusObjectPath prompt = reply.arguments().value(1).value<QDBusObjectPath>();
    if (prompt.path() == "/")
        return then(true);
    // Answered once, by Completed.
    auto* w = new SecretPromptWaiter(std::move(then));
    bus.connect(kSecrets, prompt.path(), "org.freedesktop.Secret.Prompt", "Completed", w,
                SLOT(completed(bool, QDBusVariant)));
    QDBusInterface(kSecrets, prompt.path(), "org.freedesktop.Secret.Prompt", bus).asyncCall("Prompt", QString());
}

} // namespace

} // namespace atrium

Q_DECLARE_METATYPE(atrium::Secret)

namespace atrium {

uint SecretAdaptor::RetrieveSecret(const QDBusObjectPath&, const QString& app, const QDBusUnixFileDescriptor& fd,
                                   const QVariantMap&, QVariantMap&) {
    qDBusRegisterMetaType<Secret>();
    qDBusRegisterMetaType<Paths>();
    qDBusRegisterMetaType<StringMap>();
    qDBusRegisterMetaType<SecretMap>();
    if (!fd.isValid() || app.isEmpty())
        return 2;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface service(kSecrets, kBase, kService, bus);
    const QDBusMessage opened = service.call("OpenSession", "plain", QVariant::fromValue(QDBusVariant(QString())));
    if (opened.type() != QDBusMessage::ReplyMessage)
        return 2;
    const QDBusObjectPath session = opened.arguments().value(1).value<QDBusObjectPath>();
    const StringMap attributes{{"server", "xdg-desktop-portal"}, {"type", "binary"}, {"user", app}};

    PortalBackend* backend = PortalBackend::instance();
    const QDBusMessage call = backend->delayReply();
    auto out = std::make_shared<QDBusUnixFileDescriptor>(fd);
    auto answer = [call, out, session](uint response, const QByteArray& secret) {
        if (response == 0 && !write_all(out->fileDescriptor(), secret))
            response = 2;
        QDBusConnection bus = QDBusConnection::sessionBus();
        QDBusInterface(kSecrets, session.path(), "org.freedesktop.Secret.Session", bus).asyncCall("Close");
        bus.send(call.createReply(QVariantList{response, QVariantMap{}}));
    };

    const QDBusMessage found = service.call("SearchItems", QVariant::fromValue(attributes));
    Paths items = cast<Paths>(found.arguments().value(0));
    items += cast<Paths>(found.arguments().value(1));
    if (!items.isEmpty()) {
        // The app's key already: read (opening the keyring first if it's shut).
        const QDBusObjectPath item = items.first();
        unlocked({item}, [item, session, answer](bool ok) {
            if (!ok)
                return answer(1, {});
            QDBusInterface svc(kSecrets, kBase, kService, QDBusConnection::sessionBus());
            const QDBusMessage got = svc.call("GetSecrets", QVariant::fromValue(Paths{item}), QVariant::fromValue(session));
            const SecretMap secrets = cast<SecretMap>(got.arguments().value(0));
            if (!secrets.contains(item))
                return answer(2, {});
            answer(0, secrets.value(item).value);
        });
        return 0;  // unused: the reply goes later
    }
    // None yet: a new random one, in the default keyring.
    QByteArray key(64, '\0');
    if (getrandom(key.data(), size_t(key.size()), 0) != key.size())
        return answer(2, {}), 0;
    const QDBusObjectPath collection = QDBusReply<QDBusObjectPath>(service.call("ReadAlias", "default")).value();
    if (collection.path() == "/" || collection.path().isEmpty())
        return answer(2, {}), 0;
    unlocked({collection}, [collection, attributes, session, key, app, answer](bool ok) {
        if (!ok)
            return answer(1, {});
        QDBusInterface c(kSecrets, collection.path(), "org.freedesktop.Secret.Collection", QDBusConnection::sessionBus());
        const QVariantMap props{{"org.freedesktop.Secret.Item.Label", "xdg-desktop-portal/" + app},
                                {"org.freedesktop.Secret.Item.Attributes", QVariant::fromValue(attributes)}};
        const QDBusMessage made = c.call("CreateItem", props,
                                         QVariant::fromValue(Secret{session, {}, key, "application/octet-stream"}), false);
        answer(made.type() == QDBusMessage::ReplyMessage ? 0 : 2, key);
    });
    return 0;
}

} // namespace atrium
