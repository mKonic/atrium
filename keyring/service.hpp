#pragma once
// The Secret Service (org.freedesktop.secrets) on the session bus: what
// browsers, mail and chat apps keep their passwords and keys in, through
// libsecret. Collections are keyring_core's files in
// ~/.local/share/atrium/keyrings; "login" is the default, opened by the
// login password (from pam_atrium_keyring, or asked for once). Others are
// opened by a random password kept in "login", as gnome-keyring does.
//
// One QDBusVirtualObject answers for the whole tree: the service, its
// collections (and their aliases), items, sessions and prompts.

#include "keyring_core.hpp"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QMap>
#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class QDBusServiceWatcher;

namespace atrium::keyring {

struct DBusSecret {
    QDBusObjectPath session;
    QByteArray parameters, value;
    QString content_type;
};

class Service : public QDBusVirtualObject {
    Q_OBJECT

public:
    // `dir`: where the collections live. `login_password`: the one the
    // user logged in with, when PAM handed it over.
    Service(const QString& dir, std::optional<std::string> login_password, QObject* parent = nullptr);
    ~Service() override;

    // Takes org.freedesktop.secrets: copies what the one holding it has
    // first (the first time), then waits for it to go and takes over.
    bool start();

    QString introspect(const QString& path) const override;
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override;

    // The password dialog's answer (tests drive it here).
    using Ask = std::function<void(const QString& mode, const QString& who, bool wrong,
                                   std::function<void(std::optional<std::string>)> answer)>;
    void set_ask(Ask ask) { ask_ = std::move(ask); }

private:
    struct Open {
        Collection c;
        QString file;
    };
    struct SessionEntry {
        Session s;
        QString owner;  // the client's unique name
    };
    struct PromptEntry {
        std::function<void(QString window)> run;
        std::function<void()> dismiss;
    };
    enum class Kind { None, Service, Collection, Item, Session, Prompt };
    struct Target {
        Kind kind = Kind::None;
        QString collection;  // its name (an alias resolved)
        uint64_t item = 0;
        QString id;          // session or prompt
    };

    Target resolve(const QString& path) const;
    QString collection_path(const QString& name) const;
    QString item_path(const QString& collection, uint64_t id) const;
    Open* collection(const QString& name);
    bool save(const QString& name);
    void load_all();

    // Messages, by what they're for.
    QDBusMessage service_call(const QDBusMessage& m);
    QDBusMessage collection_call(const Target& t, const QDBusMessage& m);
    QDBusMessage item_call(const Target& t, const QDBusMessage& m);
    QDBusMessage properties_call(const Target& t, const QDBusMessage& m);
    QVariantMap properties(const Target& t, const QString& interface) const;

    // Unlocking: what can be at once (the login password, a collection's
    // password in "login"), else a prompt; `done` with what got unlocked.
    // The prompt's result is `asked` (what the client named: collections or
    // items) as far as it opened, or the collections when that's empty.
    QDBusObjectPath unlock(const QStringList& names, const QString& who,
                           std::function<void(QList<QDBusObjectPath>)> done, QList<QDBusObjectPath>* now,
                           const QList<QDBusObjectPath>& asked = {});
    bool unlock_quietly(const QString& name);
    QDBusObjectPath add_prompt(PromptEntry p);
    void complete(const QString& prompt, bool dismissed, const QVariant& result);
    std::optional<std::string> collection_password(const QString& name);
    QString new_collection(const QString& label, const QString& alias);
    void emit_signal(const QString& path, const QString& interface, const QString& name, const QVariantList& args);
    QString who(const QDBusMessage& m) const;

    void migrate(const QString& from);
    void bring_up_kwallet();

    QString dir_;
    std::optional<std::string> login_password_;
    std::map<QString, Open> collections_;
    QMap<QString, QString> aliases_;  // alias: collection name
    std::map<QString, SessionEntry> sessions_;
    std::map<QString, PromptEntry> prompts_;
    uint64_t next_session_ = 1, next_prompt_ = 1;
    QDBusServiceWatcher* clients_ = nullptr;
    Ask ask_;
};

} // namespace atrium::keyring
