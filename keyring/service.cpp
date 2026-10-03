#include "service.hpp"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <csignal>

Q_DECLARE_METATYPE(atrium::keyring::DBusSecret)

namespace atrium::keyring {

using StringMap = QMap<QString, QString>;
using Paths = QList<QDBusObjectPath>;
using SecretMap = QMap<QDBusObjectPath, DBusSecret>;

QDBusArgument& operator<<(QDBusArgument& a, const DBusSecret& s) {
    a.beginStructure();
    a << s.session << s.parameters << s.value << s.content_type;
    a.endStructure();
    return a;
}

const QDBusArgument& operator>>(const QDBusArgument& a, DBusSecret& s) {
    a.beginStructure();
    a >> s.session >> s.parameters >> s.value >> s.content_type;
    a.endStructure();
    return a;
}

namespace {

const QString kBase = QStringLiteral("/org/freedesktop/secrets");
const QString kService = QStringLiteral("org.freedesktop.Secret.Service");
const QString kCollection = QStringLiteral("org.freedesktop.Secret.Collection");
const QString kItem = QStringLiteral("org.freedesktop.Secret.Item");
const QString kSession = QStringLiteral("org.freedesktop.Secret.Session");
const QString kPrompt = QStringLiteral("org.freedesktop.Secret.Prompt");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kIntrospectable = QStringLiteral("org.freedesktop.DBus.Introspectable");
const QString kIsLocked = QStringLiteral("org.freedesktop.Secret.Error.IsLocked");
const QString kNoSession = QStringLiteral("org.freedesktop.Secret.Error.NoSession");
const QString kNoSuchObject = QStringLiteral("org.freedesktop.Secret.Error.NoSuchObject");
const QString kLogin = QStringLiteral("login");
// A collection's password, kept in "login".
const char* kPasswordAttr = "atrium-keyring";

int64_t now() {
    return QDateTime::currentSecsSinceEpoch();
}

template <class T>
T cast(const QVariant& v) {
    if (v.canConvert<QDBusArgument>())
        return qdbus_cast<T>(v.value<QDBusArgument>());
    return v.value<T>();
}

Attributes to_attributes(const StringMap& m) {
    Attributes a;
    for (auto it = m.begin(); it != m.end(); ++it)
        a[it.key().toStdString()] = it.value().toStdString();
    return a;
}

StringMap from_attributes(const Attributes& a) {
    StringMap m;
    for (const auto& [k, v] : a)
        m.insert(QString::fromStdString(k), QString::fromStdString(v));
    return m;
}

Bytes to_bytes(const QByteArray& b) {
    return Bytes(b.begin(), b.end());
}

QByteArray from_bytes(const Bytes& b) {
    return QByteArray(reinterpret_cast<const char*>(b.data()), qsizetype(b.size()));
}

// "My Keyring" → "my_keyring": a path element.
QString path_name(const QString& label) {
    QString s = label.toLower();
    s.replace(QRegularExpression("[^a-z0-9_]"), "_");
    return s.isEmpty() ? QStringLiteral("collection") : s;
}

const char* kIntrospectXml = R"xml(
  <interface name="org.freedesktop.Secret.Service">
    <property name="Collections" type="ao" access="read"/>
    <method name="OpenSession"><arg name="algorithm" type="s" direction="in"/><arg name="input" type="v" direction="in"/><arg name="output" type="v" direction="out"/><arg name="result" type="o" direction="out"/></method>
    <method name="CreateCollection"><arg name="properties" type="a{sv}" direction="in"/><arg name="alias" type="s" direction="in"/><arg name="collection" type="o" direction="out"/><arg name="prompt" type="o" direction="out"/></method>
    <method name="SearchItems"><arg name="attributes" type="a{ss}" direction="in"/><arg name="unlocked" type="ao" direction="out"/><arg name="locked" type="ao" direction="out"/></method>
    <method name="Unlock"><arg name="objects" type="ao" direction="in"/><arg name="unlocked" type="ao" direction="out"/><arg name="prompt" type="o" direction="out"/></method>
    <method name="Lock"><arg name="objects" type="ao" direction="in"/><arg name="locked" type="ao" direction="out"/><arg name="Prompt" type="o" direction="out"/></method>
    <method name="GetSecrets"><arg name="items" type="ao" direction="in"/><arg name="session" type="o" direction="in"/><arg name="secrets" type="a{o(oayays)}" direction="out"/></method>
    <method name="ReadAlias"><arg name="name" type="s" direction="in"/><arg name="collection" type="o" direction="out"/></method>
    <method name="SetAlias"><arg name="name" type="s" direction="in"/><arg name="collection" type="o" direction="in"/></method>
    <signal name="CollectionCreated"><arg name="collection" type="o"/></signal>
    <signal name="CollectionDeleted"><arg name="collection" type="o"/></signal>
    <signal name="CollectionChanged"><arg name="collection" type="o"/></signal>
  </interface>)xml";

const char* kCollectionXml = R"xml(
  <interface name="org.freedesktop.Secret.Collection">
    <property name="Items" type="ao" access="read"/>
    <property name="Label" type="s" access="readwrite"/>
    <property name="Locked" type="b" access="read"/>
    <property name="Created" type="t" access="read"/>
    <property name="Modified" type="t" access="read"/>
    <method name="Delete"><arg name="prompt" type="o" direction="out"/></method>
    <method name="SearchItems"><arg name="attributes" type="a{ss}" direction="in"/><arg name="results" type="ao" direction="out"/></method>
    <method name="CreateItem"><arg name="properties" type="a{sv}" direction="in"/><arg name="secret" type="(oayays)" direction="in"/><arg name="replace" type="b" direction="in"/><arg name="item" type="o" direction="out"/><arg name="prompt" type="o" direction="out"/></method>
    <signal name="ItemCreated"><arg name="item" type="o"/></signal>
    <signal name="ItemDeleted"><arg name="item" type="o"/></signal>
    <signal name="ItemChanged"><arg name="item" type="o"/></signal>
  </interface>)xml";

const char* kItemXml = R"xml(
  <interface name="org.freedesktop.Secret.Item">
    <property name="Locked" type="b" access="read"/>
    <property name="Attributes" type="a{ss}" access="readwrite"/>
    <property name="Label" type="s" access="readwrite"/>
    <property name="Created" type="t" access="read"/>
    <property name="Modified" type="t" access="read"/>
    <method name="Delete"><arg name="Prompt" type="o" direction="out"/></method>
    <method name="GetSecret"><arg name="session" type="o" direction="in"/><arg name="secret" type="(oayays)" direction="out"/></method>
    <method name="SetSecret"><arg name="secret" type="(oayays)" direction="in"/></method>
  </interface>)xml";

const char* kSessionXml = R"xml(
  <interface name="org.freedesktop.Secret.Session">
    <method name="Close"/>
  </interface>)xml";

const char* kPromptXml = R"xml(
  <interface name="org.freedesktop.Secret.Prompt">
    <method name="Prompt"><arg name="window-id" type="s" direction="in"/></method>
    <method name="Dismiss"/>
    <signal name="Completed"><arg name="dismissed" type="b"/><arg name="result" type="v"/></signal>
  </interface>)xml";

} // namespace

Service::Service(const QString& dir, std::optional<std::string> login_password, QObject* parent)
    : QDBusVirtualObject(parent), dir_(dir), login_password_(std::move(login_password)) {
    qDBusRegisterMetaType<DBusSecret>();
    qDBusRegisterMetaType<StringMap>();
    qDBusRegisterMetaType<Paths>();
    qDBusRegisterMetaType<SecretMap>();
    QDir().mkpath(dir_);
    QFile::setPermissions(dir_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    load_all();
}

Service::~Service() = default;

// --- files ---------------------------------------------------------------------------

void Service::load_all() {
    const QDir d(dir_);
    for (const QString& f : d.entryList({"*.keyring"}, QDir::Files)) {
        QFile file(d.filePath(f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        std::string why;
        auto c = Collection::parse(file.readAll().toStdString(), &why);
        if (!c) {
            qWarning("atrium-keyring: %s: %s", qPrintable(f), why.c_str());
            continue;
        }
        collections_.emplace(f.chopped(8), Open{std::move(*c), d.filePath(f)});
    }
    QFile a(d.filePath("aliases.json"));
    if (a.open(QIODevice::ReadOnly)) {
        const QJsonObject o = QJsonDocument::fromJson(a.readAll()).object();
        for (auto it = o.begin(); it != o.end(); ++it)
            aliases_.insert(it.key(), it.value().toString());
    }
    if (!aliases_.contains("default"))
        aliases_.insert("default", kLogin);

    // The login password opens "login" (or makes it, the first time).
    Open* login = collection(kLogin);
    if (!login && login_password_) {
        collections_.emplace(kLogin, Open{Collection::create("Login", *login_password_, now()),
                                          d.filePath("login.keyring")});
        save(kLogin);
    } else if (login && login_password_ && !login->c.unlock(*login_password_)) {
        // Made without a password (no login password then): it takes this one now.
        if (login->c.unlock("")) {
            login->c.set_password(*login_password_);
            save(kLogin);
        } else {
            qWarning("atrium-keyring: the login password doesn't open the keyring (it was changed?); "
                     "it will ask for the old one");
        }
    }
}

bool Service::save(const QString& name) {
    Open* o = collection(name);
    if (!o)
        return false;
    const auto text = o->c.serialize();
    if (!text)
        return false;
    QSaveFile f(o->file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(text->data(), qint64(text->size()));
    return f.commit();
}

Service::Open* Service::collection(const QString& name) {
    auto it = collections_.find(name);
    return it == collections_.end() ? nullptr : &it->second;
}

// --- paths ---------------------------------------------------------------------------

QString Service::collection_path(const QString& name) const {
    return kBase + "/collection/" + name;
}

QString Service::item_path(const QString& collection, uint64_t id) const {
    return collection_path(collection) + "/" + QString::number(id);
}

Service::Target Service::resolve(const QString& path) const {
    Target t;
    if (path == kBase) {
        t.kind = Kind::Service;
        return t;
    }
    if (!path.startsWith(kBase + "/"))
        return t;
    const QStringList parts = path.mid(kBase.size() + 1).split('/');
    if (parts.size() == 2 && parts[0] == "session") {
        if (sessions_.contains(parts[1]))
            t = {Kind::Session, {}, 0, parts[1]};
        return t;
    }
    if (parts.size() == 2 && parts[0] == "prompt") {
        if (prompts_.contains(parts[1]))
            t = {Kind::Prompt, {}, 0, parts[1]};
        return t;
    }
    if ((parts.size() == 2 || parts.size() == 3) && (parts[0] == "collection" || parts[0] == "aliases")) {
        const QString name = parts[0] == "aliases" ? aliases_.value(parts[1]) : parts[1];
        auto it = collections_.find(name);
        if (it == collections_.end())
            return t;
        if (parts.size() == 2)
            return {Kind::Collection, name, 0, {}};
        bool ok = false;
        const uint64_t id = parts[2].toULongLong(&ok);
        const auto ids = it->second.c.ids();
        if (ok && std::ranges::find(ids, id) != ids.end())
            return {Kind::Item, name, id, {}};
    }
    return t;
}

// --- D-Bus -----------------------------------------------------------------------------

QString Service::introspect(const QString& path) const {
    const Target t = resolve(path);
    QString xml;
    switch (t.kind) {
    case Kind::Service: xml = kIntrospectXml; break;
    case Kind::Collection: xml = kCollectionXml; break;
    case Kind::Item: xml = kItemXml; break;
    case Kind::Session: xml = kSessionXml; break;
    case Kind::Prompt: xml = kPromptXml; break;
    case Kind::None: break;
    }
    if (t.kind != Kind::None && t.kind != Kind::Session && t.kind != Kind::Prompt)
        xml += R"xml(
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
    <method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method>
    <method name="Set"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/></method>
  </interface>)xml";
    // Children, for tools that walk the tree.
    if (t.kind == Kind::Service) {
        for (const auto& [name, o] : collections_)
            xml += QString("\n  <node name=\"collection/%1\"/>").arg(name);
    } else if (t.kind == Kind::Collection) {
        for (uint64_t id : collections_.at(t.collection).c.ids())
            xml += QString("\n  <node name=\"%1\"/>").arg(id);
    }
    return xml;
}

QString Service::who(const QDBusMessage& m) const {
    const QDBusReply<uint> pid = QDBusConnection::sessionBus().interface()->servicePid(m.service());
    if (!pid.isValid())
        return {};
    QFile comm(QString("/proc/%1/comm").arg(pid.value()));
    if (!comm.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(comm.readAll()).trimmed();
}

bool Service::handleMessage(const QDBusMessage& m, const QDBusConnection& connection) {
    const Target t = resolve(m.path());
    QDBusMessage reply;
    if (t.kind == Kind::None) {
        reply = m.createErrorReply(kNoSuchObject, "No such object: " + m.path());
    } else if (m.interface() == kProps) {
        reply = properties_call(t, m);
    } else if (t.kind == Kind::Service && (m.interface() == kService || m.interface().isEmpty())) {
        reply = service_call(m);
    } else if (t.kind == Kind::Collection && (m.interface() == kCollection || m.interface().isEmpty())) {
        reply = collection_call(t, m);
    } else if (t.kind == Kind::Item && (m.interface() == kItem || m.interface().isEmpty())) {
        reply = item_call(t, m);
    } else if (t.kind == Kind::Session && m.member() == "Close") {
        sessions_.erase(t.id);
        reply = m.createReply();
    } else if (t.kind == Kind::Prompt && m.member() == "Prompt") {
        auto run = prompts_.at(t.id).run;
        reply = m.createReply();
        connection.send(reply);
        run(m.arguments().value(0).toString());
        return true;
    } else if (t.kind == Kind::Prompt && m.member() == "Dismiss") {
        if (auto d = prompts_.at(t.id).dismiss)
            d();
        complete(t.id, true, QVariant::fromValue(QDBusVariant(QString())));
        reply = m.createReply();
    } else {
        return false;
    }
    if (reply.type() == QDBusMessage::InvalidMessage)
        return false;
    connection.send(reply);
    return true;
}

QVariantMap Service::properties(const Target& t, const QString& interface) const {
    QVariantMap p;
    if (t.kind == Kind::Service && (interface.isEmpty() || interface == kService)) {
        Paths all;
        for (const auto& [name, o] : collections_)
            all << QDBusObjectPath(collection_path(name));
        p["Collections"] = QVariant::fromValue(all);
    } else if (t.kind == Kind::Collection && (interface.isEmpty() || interface == kCollection)) {
        const Collection& c = collections_.at(t.collection).c;
        Paths items;
        for (uint64_t id : c.ids())
            items << QDBusObjectPath(item_path(t.collection, id));
        p["Items"] = QVariant::fromValue(items);
        p["Label"] = QString::fromStdString(c.label);
        p["Locked"] = c.locked();
        p["Created"] = quint64(c.created);
        p["Modified"] = quint64(c.modified);
    } else if (t.kind == Kind::Item && (interface.isEmpty() || interface == kItem)) {
        const Collection& c = collections_.at(t.collection).c;
        const Item* i = c.item(t.item);
        p["Locked"] = c.locked();
        p["Label"] = i ? QString::fromStdString(i->label) : QString();
        p["Attributes"] = QVariant::fromValue(i ? from_attributes(i->attributes) : StringMap{});
        p["Created"] = quint64(i ? i->created : 0);
        p["Modified"] = quint64(i ? i->modified : 0);
    }
    return p;
}

QDBusMessage Service::properties_call(const Target& t, const QDBusMessage& m) {
    const QVariantList a = m.arguments();
    if (m.member() == "GetAll")
        return m.createReply(QVariant::fromValue(properties(t, a.value(0).toString())));
    if (m.member() == "Get") {
        const QVariantMap p = properties(t, a.value(0).toString());
        if (!p.contains(a.value(1).toString()))
            return m.createErrorReply(QDBusError::InvalidArgs, "No such property");
        return m.createReply(QVariant::fromValue(QDBusVariant(p.value(a.value(1).toString()))));
    }
    if (m.member() == "Set") {
        const QString name = a.value(1).toString();
        const QVariant v = a.value(2).value<QDBusVariant>().variant();
        Open* o = collection(t.collection);
        if (!o)
            return m.createErrorReply(QDBusError::PropertyReadOnly, "Read-only");
        if (o->c.locked())
            return m.createErrorReply(kIsLocked, "Locked");
        if (t.kind == Kind::Collection && name == "Label") {
            o->c.label = v.toString().toStdString();
            save(t.collection);
            emit_signal(kBase, kService, "CollectionChanged", {QVariant::fromValue(QDBusObjectPath(collection_path(t.collection)))});
            return m.createReply();
        }
        if (t.kind == Kind::Item && (name == "Label" || name == "Attributes")) {
            Item i = *o->c.item(t.item);
            if (name == "Label")
                i.label = v.toString().toStdString();
            else
                i.attributes = to_attributes(cast<StringMap>(v));
            o->c.update(i, now());
            save(t.collection);
            emit_signal(collection_path(t.collection), kCollection, "ItemChanged",
                        {QVariant::fromValue(QDBusObjectPath(item_path(t.collection, t.item)))});
            return m.createReply();
        }
        return m.createErrorReply(QDBusError::PropertyReadOnly, "Read-only");
    }
    return m.createErrorReply(QDBusError::UnknownMethod, m.member());
}

QDBusMessage Service::service_call(const QDBusMessage& m) {
    const QVariantList a = m.arguments();
    const QString member = m.member();
    if (member == "OpenSession") {
        const QVariant input = a.value(1).value<QDBusVariant>().variant();
        Bytes out;
        auto s = Session::open(a.value(0).toString().toStdString(), to_bytes(input.toByteArray()), &out);
        if (!s)
            return m.createErrorReply(QDBusError::NotSupported, "Algorithm not supported");
        const QString id = QString::number(next_session_++);
        sessions_.emplace(id, SessionEntry{std::move(*s), m.service()});
        if (!clients_) {
            clients_ = new QDBusServiceWatcher(this);
            clients_->setConnection(QDBusConnection::sessionBus());
            clients_->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
            connect(clients_, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString& name) {
                clients_->removeWatchedService(name);
                std::erase_if(sessions_, [&](const auto& e) { return e.second.owner == name; });
            });
        }
        clients_->addWatchedService(m.service());
        const QVariant output = out.empty() ? QVariant(QString()) : QVariant(from_bytes(out));
        return m.createReply(
            {QVariant::fromValue(QDBusVariant(output)), QVariant::fromValue(QDBusObjectPath(kBase + "/session/" + id))});
    }
    if (member == "SearchItems") {
        const Attributes match = to_attributes(cast<StringMap>(a.value(0)));
        Paths unlocked, locked;
        for (const auto& [name, o] : collections_)
            for (uint64_t id : o.c.search(match))
                (o.c.locked() ? locked : unlocked) << QDBusObjectPath(item_path(name, id));
        return m.createReply({QVariant::fromValue(unlocked), QVariant::fromValue(locked)});
    }
    if (member == "Unlock") {
        QStringList names;
        Paths already;
        for (const QDBusObjectPath& p : cast<Paths>(a.value(0))) {
            const Target t = resolve(p.path());
            if (t.kind != Kind::Collection && t.kind != Kind::Item)
                continue;
            if (!collections_.at(t.collection).c.locked())
                already << p;
            else if (!names.contains(t.collection))
                names << t.collection;
        }
        const Paths asked = cast<Paths>(a.value(0));
        QList<QDBusObjectPath> now_unlocked;
        const QDBusObjectPath prompt = unlock(names, who(m), [](Paths) {}, &now_unlocked, asked);
        // What's open now, of what was asked: the objects themselves.
        Paths out = already;
        for (const QDBusObjectPath& p : asked) {
            const Target t = resolve(p.path());
            if ((t.kind == Kind::Collection || t.kind == Kind::Item) && !already.contains(p) &&
                !collections_.at(t.collection).c.locked())
                out << p;
        }
        return m.createReply({QVariant::fromValue(out), QVariant::fromValue(prompt)});
    }
    if (member == "Lock") {
        Paths locked;
        for (const QDBusObjectPath& p : cast<Paths>(a.value(0))) {
            const Target t = resolve(p.path());
            if (t.kind == Kind::Collection || t.kind == Kind::Item) {
                save(t.collection);
                collections_.at(t.collection).c.lock();
                locked << p;
            }
        }
        return m.createReply({QVariant::fromValue(locked), QVariant::fromValue(QDBusObjectPath("/"))});
    }
    if (member == "GetSecrets") {
        const auto s = sessions_.find(a.value(1).value<QDBusObjectPath>().path().section('/', -1));
        if (s == sessions_.end())
            return m.createErrorReply(kNoSession, "No such session");
        SecretMap out;
        for (const QDBusObjectPath& p : cast<Paths>(a.value(0))) {
            const Target t = resolve(p.path());
            if (t.kind != Kind::Item)
                continue;
            const Item* i = collections_.at(t.collection).c.item(t.item);
            if (!i)
                continue;  // locked
            Bytes params, value;
            s->second.s.encrypt(i->secret, &params, &value);
            out.insert(p, DBusSecret{a.value(1).value<QDBusObjectPath>(), from_bytes(params), from_bytes(value),
                                     QString::fromStdString(i->content_type)});
        }
        return m.createReply(QVariant::fromValue(out));
    }
    if (member == "ReadAlias") {
        const QString name = aliases_.value(a.value(0).toString());
        return m.createReply(QVariant::fromValue(
            QDBusObjectPath(collections_.contains(name) ? collection_path(name) : QStringLiteral("/"))));
    }
    if (member == "SetAlias") {
        const QString alias = a.value(0).toString();
        const Target t = resolve(a.value(1).value<QDBusObjectPath>().path());
        if (t.kind == Kind::Collection)
            aliases_.insert(alias, t.collection);
        else
            aliases_.remove(alias);
        QJsonObject o;
        for (auto it = aliases_.begin(); it != aliases_.end(); ++it)
            o.insert(it.key(), it.value());
        QSaveFile f(QDir(dir_).filePath("aliases.json"));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(o).toJson());
            f.commit();
        }
        return m.createReply();
    }
    if (member == "CreateCollection") {
        const QVariantMap props = cast<QVariantMap>(a.value(0));
        const QString label = props.value(kCollection + ".Label").toString();
        const QString alias = a.value(1).toString();
        // An alias already there: that one (as the spec says).
        if (!alias.isEmpty() && collections_.contains(aliases_.value(alias)))
            return m.createReply({QVariant::fromValue(QDBusObjectPath(collection_path(aliases_.value(alias)))),
                                  QVariant::fromValue(QDBusObjectPath("/"))});
        const QString made = new_collection(label, alias);
        if (!made.isEmpty())
            return m.createReply({QVariant::fromValue(QDBusObjectPath(collection_path(made))),
                                  QVariant::fromValue(QDBusObjectPath("/"))});
        // "login" first: made (or opened) through a prompt, then this.
        const QString w = who(m);
        const QDBusObjectPath prompt = add_prompt({});
        const QString id = prompt.path().section('/', -1);
        prompts_.at(id).run = [this, id, label, alias, w](QString) {
            QList<QDBusObjectPath> ignored;
            const QDBusObjectPath inner = unlock({kLogin}, w, [this, id, label, alias](Paths opened) {
                const QString made = opened.isEmpty() ? QString() : new_collection(label, alias);
                if (made.isEmpty())
                    complete(id, true, QVariant::fromValue(QDBusVariant(QVariant::fromValue(QDBusObjectPath("/")))));
                else
                    complete(id, false,
                             QVariant::fromValue(QDBusVariant(QVariant::fromValue(QDBusObjectPath(collection_path(made))))));
            }, &ignored);
            if (inner.path() != "/")
                prompts_.at(inner.path().section('/', -1)).run({});
            else if (!ignored.isEmpty() || !collections_.at(kLogin).c.locked()) {
                const QString made = new_collection(label, alias);
                complete(id, made.isEmpty(), QVariant::fromValue(QDBusVariant(QVariant::fromValue(
                                                 QDBusObjectPath(made.isEmpty() ? "/" : collection_path(made))))));
            }
        };
        return m.createReply({QVariant::fromValue(QDBusObjectPath("/")), QVariant::fromValue(prompt)});
    }
    return m.createErrorReply(QDBusError::UnknownMethod, member);
}

QDBusMessage Service::collection_call(const Target& t, const QDBusMessage& m) {
    const QVariantList a = m.arguments();
    Open& o = collections_.at(t.collection);
    if (m.member() == "SearchItems") {
        Paths out;
        for (uint64_t id : o.c.search(to_attributes(cast<StringMap>(a.value(0)))))
            out << QDBusObjectPath(item_path(t.collection, id));
        return m.createReply(QVariant::fromValue(out));
    }
    if (m.member() == "CreateItem") {
        if (o.c.locked())
            return m.createErrorReply(kIsLocked, "The collection is locked");
        const QVariantMap props = cast<QVariantMap>(a.value(0));
        const DBusSecret secret = cast<DBusSecret>(a.value(1));
        const auto s = sessions_.find(secret.session.path().section('/', -1));
        if (s == sessions_.end())
            return m.createErrorReply(kNoSession, "No such session");
        auto plain = s->second.s.decrypt(to_bytes(secret.parameters), to_bytes(secret.value));
        if (!plain)
            return m.createErrorReply(QDBusError::InvalidArgs, "The secret doesn't decrypt");
        Item i;
        i.label = props.value(kItem + ".Label").toString().toStdString();
        i.attributes = to_attributes(cast<StringMap>(props.value(kItem + ".Attributes")));
        i.secret = std::move(*plain);
        i.content_type = secret.content_type.isEmpty() ? "text/plain" : secret.content_type.toStdString();
        const auto before = o.c.ids();
        const uint64_t id = o.c.put(std::move(i), a.value(2).toBool(), now());
        save(t.collection);
        const QDBusObjectPath path(item_path(t.collection, id));
        const bool fresh = std::ranges::find(before, id) == before.end();
        emit_signal(collection_path(t.collection), kCollection, fresh ? "ItemCreated" : "ItemChanged",
                    {QVariant::fromValue(path)});
        return m.createReply({QVariant::fromValue(path), QVariant::fromValue(QDBusObjectPath("/"))});
    }
    if (m.member() == "Delete") {
        if (t.collection == kLogin)
            return m.createErrorReply(QDBusError::AccessDenied, "The login keyring stays");
        QFile::remove(o.file);
        const QDBusObjectPath path(collection_path(t.collection));
        collections_.erase(t.collection);
        emit_signal(kBase, kService, "CollectionDeleted", {QVariant::fromValue(path)});
        return m.createReply(QVariant::fromValue(QDBusObjectPath("/")));
    }
    return m.createErrorReply(QDBusError::UnknownMethod, m.member());
}

QDBusMessage Service::item_call(const Target& t, const QDBusMessage& m) {
    const QVariantList a = m.arguments();
    Open& o = collections_.at(t.collection);
    if (m.member() == "GetSecret") {
        const auto s = sessions_.find(a.value(0).value<QDBusObjectPath>().path().section('/', -1));
        if (s == sessions_.end())
            return m.createErrorReply(kNoSession, "No such session");
        const Item* i = o.c.item(t.item);
        if (!i)
            return m.createErrorReply(kIsLocked, "The item is locked");
        Bytes params, value;
        s->second.s.encrypt(i->secret, &params, &value);
        return m.createReply(QVariant::fromValue(DBusSecret{a.value(0).value<QDBusObjectPath>(), from_bytes(params),
                                                            from_bytes(value), QString::fromStdString(i->content_type)}));
    }
    if (m.member() == "SetSecret") {
        const Item* i = o.c.item(t.item);
        if (!i)
            return m.createErrorReply(kIsLocked, "The item is locked");
        const DBusSecret secret = cast<DBusSecret>(a.value(0));
        const auto s = sessions_.find(secret.session.path().section('/', -1));
        if (s == sessions_.end())
            return m.createErrorReply(kNoSession, "No such session");
        auto plain = s->second.s.decrypt(to_bytes(secret.parameters), to_bytes(secret.value));
        if (!plain)
            return m.createErrorReply(QDBusError::InvalidArgs, "The secret doesn't decrypt");
        Item changed = *i;
        changed.secret = std::move(*plain);
        if (!secret.content_type.isEmpty())
            changed.content_type = secret.content_type.toStdString();
        o.c.update(changed, now());
        save(t.collection);
        emit_signal(collection_path(t.collection), kCollection, "ItemChanged",
                    {QVariant::fromValue(QDBusObjectPath(m.path()))});
        return m.createReply();
    }
    if (m.member() == "Delete") {
        if (o.c.locked())
            return m.createErrorReply(kIsLocked, "The item is locked");
        o.c.remove(t.item, now());
        save(t.collection);
        emit_signal(collection_path(t.collection), kCollection, "ItemDeleted",
                    {QVariant::fromValue(QDBusObjectPath(item_path(t.collection, t.item)))});
        return m.createReply(QVariant::fromValue(QDBusObjectPath("/")));
    }
    return m.createErrorReply(QDBusError::UnknownMethod, m.member());
}

void Service::emit_signal(const QString& path, const QString& interface, const QString& name, const QVariantList& args) {
    QDBusMessage s = QDBusMessage::createSignal(path, interface, name);
    s.setArguments(args);
    QDBusConnection::sessionBus().send(s);
}

// --- unlocking -------------------------------------------------------------------------

std::optional<std::string> Service::collection_password(const QString& name) {
    Open* login = collection(kLogin);
    if (!login || login->c.locked())
        return std::nullopt;
    const auto ids = login->c.search({{kPasswordAttr, "collection-password"}, {"collection", name.toStdString()}});
    if (ids.empty())
        return std::nullopt;
    const Bytes& s = login->c.item(ids.front())->secret;
    return std::string(s.begin(), s.end());
}

bool Service::unlock_quietly(const QString& name) {
    Open* o = collection(name);
    if (!o)
        return false;
    if (!o->c.locked())
        return true;
    if (name == kLogin)
        return login_password_ && o->c.unlock(*login_password_);
    const auto pw = collection_password(name);
    return pw && o->c.unlock(*pw);
}

QString Service::new_collection(const QString& label, const QString& alias) {
    Open* login = collection(kLogin);
    if (!login || login->c.locked())
        return {};
    QString name = path_name(label);
    for (int n = 2; collections_.contains(name); ++n)
        name = path_name(label) + QString::number(n);
    const Bytes random = random_bytes(24);
    std::string password;
    for (uint8_t b : random)
        password += "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"[b % 62];
    Item keep;
    keep.label = "Password for the \"" + label.toStdString() + "\" keyring";
    keep.attributes = {{kPasswordAttr, "collection-password"}, {"collection", name.toStdString()}};
    keep.secret = Bytes(password.begin(), password.end());
    login->c.put(std::move(keep), true, now());
    save(kLogin);
    collections_.emplace(name, Open{Collection::create(label.toStdString(), password, now()),
                                    QDir(dir_).filePath(name + ".keyring")});
    save(name);
    if (!alias.isEmpty())
        aliases_.insert(alias, name);
    emit_signal(kBase, kService, "CollectionCreated", {QVariant::fromValue(QDBusObjectPath(collection_path(name)))});
    return name;
}

QDBusObjectPath Service::add_prompt(PromptEntry p) {
    const QString id = QString::number(next_prompt_++);
    prompts_.emplace(id, std::move(p));
    return QDBusObjectPath(kBase + "/prompt/" + id);
}

void Service::complete(const QString& id, bool dismissed, const QVariant& result) {
    if (!prompts_.erase(id))
        return;
    emit_signal(kBase + "/prompt/" + id, kPrompt, "Completed", {dismissed, result});
}

QDBusObjectPath Service::unlock(const QStringList& names, const QString& who,
                                std::function<void(QList<QDBusObjectPath>)> done, QList<QDBusObjectPath>* now_open,
                                const QList<QDBusObjectPath>& asked) {
    QStringList waiting;
    for (const QString& n : names) {
        if (unlock_quietly(n))
            *now_open << QDBusObjectPath(collection_path(n));
        else
            waiting << n;
    }
    if (waiting.isEmpty())
        return QDBusObjectPath("/");
    const QDBusObjectPath prompt = add_prompt({});
    const QString id = prompt.path().section('/', -1);
    // Asks for the login keyring's password (or, with none yet, for the
    // login password to make it with); the rest open from it.
    auto ask = std::make_shared<std::function<void(bool)>>();
    *ask = [this, id, waiting, who, done, ask, asked](bool wrong) {
        Open* login = collection(kLogin);
        const QString mode = login ? QStringLiteral("unlock") : QStringLiteral("create");
        if (!ask_) {
            complete(id, true, QVariant::fromValue(QDBusVariant(QVariant::fromValue(Paths{}))));
            return;
        }
        ask_(mode, who, wrong, [this, id, waiting, done, ask, mode, asked](std::optional<std::string> pw) {
            if (!prompts_.contains(id))
                return;  // dismissed meanwhile
            if (!pw) {
                complete(id, true, QVariant::fromValue(QDBusVariant(QVariant::fromValue(Paths{}))));
                done({});
                return;
            }
            Open* login = collection(kLogin);
            if (mode == "create") {
                collections_.emplace(kLogin, Open{Collection::create("Login", *pw, now()),
                                                  QDir(dir_).filePath("login.keyring")});
                save(kLogin);
                emit_signal(kBase, kService, "CollectionCreated",
                            {QVariant::fromValue(QDBusObjectPath(collection_path(kLogin)))});
            } else if (!login->c.unlock(*pw)) {
                (*ask)(true);
                return;
            } else if (login_password_ && *pw != *login_password_) {
                // Opened by an older password: the login password opens it from now on.
                login->c.set_password(*login_password_);
                save(kLogin);
            }
            Paths opened;
            for (const QString& n : waiting)
                if (unlock_quietly(n))
                    opened << QDBusObjectPath(collection_path(n));
            Paths result;
            for (const QDBusObjectPath& p : asked) {
                const Target t = resolve(p.path());
                if ((t.kind == Kind::Collection || t.kind == Kind::Item) && !collections_.at(t.collection).c.locked())
                    result << p;
            }
            complete(id, false, QVariant::fromValue(QDBusVariant(QVariant::fromValue(asked.isEmpty() ? opened : result))));
            done(opened);
        });
    };
    prompts_.at(id).run = [ask](QString) { (*ask)(false); };
    return prompt;
}

// --- taking over -------------------------------------------------------------------------

// Everything the Secret Service that ran before has: copied into "login"
// (once), so Chrome's and the rest's keys come along.
void Service::migrate(const QString& from) {
    Open* login = collection(kLogin);
    if (!login || login->c.locked())
        return;
    const QString done_flag = QDir(dir_).filePath("migrated");
    if (QFile::exists(done_flag))
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface service(from, kBase, kService, bus);
    service.setTimeout(30000);
    QDBusMessage opened = service.call("OpenSession", "plain", QVariant::fromValue(QDBusVariant(QString())));
    if (opened.type() != QDBusMessage::ReplyMessage) {
        qWarning("atrium-keyring: can't read the old keyring: %s", qPrintable(opened.errorMessage()));
        return;
    }
    const QDBusObjectPath session = opened.arguments().value(1).value<QDBusObjectPath>();
    QDBusInterface props(from, kBase, kProps, bus);
    const Paths collections = cast<Paths>(props.call("Get", kService, "Collections").arguments().value(0)
                                              .value<QDBusVariant>().variant());
    int copied = 0, missed = 0;
    for (const QDBusObjectPath& c : collections) {
        // Opened first if it isn't, by its own prompt (KWallet's, GNOME's).
        QDBusMessage u = service.call("Unlock", QVariant::fromValue(Paths{c}));
        const QDBusObjectPath prompt = u.arguments().value(1).value<QDBusObjectPath>();
        if (u.type() == QDBusMessage::ReplyMessage && prompt.path() != "/") {
            QEventLoop wait;
            bus.connect(from, prompt.path(), kPrompt, "Completed", &wait, SLOT(quit()));
            QDBusInterface(from, prompt.path(), kPrompt, bus).call("Prompt", QString());
            QTimer::singleShot(120000, &wait, &QEventLoop::quit);
            wait.exec();
        }
        QDBusInterface cprops(from, c.path(), kProps, bus);
        const Paths items =
            cast<Paths>(cprops.call("Get", kCollection, "Items").arguments().value(0).value<QDBusVariant>().variant());
        const QDBusMessage got = service.call("GetSecrets", QVariant::fromValue(items), QVariant::fromValue(session));
        const SecretMap secrets = cast<SecretMap>(got.arguments().value(0));
        for (const QDBusObjectPath& i : items) {
            if (!secrets.contains(i)) {
                ++missed;  // still locked (a prompt dismissed), or it failed
                continue;
            }
            QDBusInterface iprops(from, i.path(), kProps, bus);
            const QVariantMap all = cast<QVariantMap>(iprops.call("GetAll", kItem).arguments().value(0));
            Item item;
            item.label = all.value("Label").toString().toStdString();
            item.attributes = to_attributes(cast<StringMap>(all.value("Attributes")));
            item.secret = to_bytes(secrets.value(i).value);
            item.content_type = secrets.value(i).content_type.isEmpty() ? "text/plain"
                                                                         : secrets.value(i).content_type.toStdString();
            item.created = int64_t(all.value("Created").toULongLong());
            item.modified = int64_t(all.value("Modified").toULongLong());
            // Not twice (a copy cut short and run again).
            bool have = false;
            for (uint64_t id : login->c.search(item.attributes))
                if (login->c.item(id)->label == item.label)
                    have = true;
            if (!have) {
                login->c.put(std::move(item), false, now());
                ++copied;
            }
        }
    }
    QDBusInterface(from, session.path(), kSession, bus).call("Close");
    save(kLogin);
    if (missed) {
        // Not done: the rest at the next login (what came over stays, once).
        qWarning("atrium-keyring: copied %d items; %d couldn't be read, tried again next time", copied, missed);
        return;
    }
    QFile flag(done_flag);
    if (flag.open(QIODevice::WriteOnly))
        flag.write(QString("%1 items\n").arg(copied).toUtf8());
    qInfo("atrium-keyring: copied %d items from the keyring that ran before", copied);
}

// A KDE wallet to copy and its Secret Service not running (atrium used to
// start it): started, the way atrium did (pam_kwallet_init, which opens it
// with the login password pam_kwallet kept), else by D-Bus (it then asks
// for its password itself); a while to come up.
void Service::bring_up_kwallet() {
    QDBusConnectionInterface* dbus = QDBusConnection::sessionBus().interface();
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!QFile::exists(data + "/kwalletd/kdewallet.kwl") || dbus->isServiceRegistered("org.freedesktop.secrets"))
        return;
    if (qEnvironmentVariableIsSet("PAM_KWALLET5_LOGIN") && QFile::exists("/usr/lib/pam_kwallet_init"))
        QProcess::startDetached("/usr/lib/pam_kwallet_init", {});
    else
        dbus->startService("org.kde.secretservicecompat");
    for (int i = 0; i < 150 && !dbus->isServiceRegistered("org.freedesktop.secrets"); ++i) {
        QEventLoop wait;
        QTimer::singleShot(100, &wait, &QEventLoop::quit);
        wait.exec();
    }
    if (!dbus->isServiceRegistered("org.freedesktop.secrets"))
        qWarning("atrium-keyring: KWallet's Secret Service didn't come up; nothing copied from it");
}

bool Service::start() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerVirtualObject(kBase, this, QDBusConnection::SubPath))
        return false;
    QDBusConnectionInterface* dbus = bus.interface();
    if (!QFile::exists(QDir(dir_).filePath("migrated")))
        bring_up_kwallet();
    const QString owner = dbus->serviceOwner("org.freedesktop.secrets");
    if (!owner.isEmpty() && !QFile::exists(QDir(dir_).filePath("migrated"))) {
        if (!collection(kLogin)) {
            // Nothing of ours yet and no login password: made with the one
            // asked for, so the old one's items have somewhere to go.
            QEventLoop wait;
            QList<QDBusObjectPath> ignored;
            const QDBusObjectPath p = unlock({kLogin}, "atrium", [&wait](Paths) { wait.quit(); }, &ignored);
            if (p.path() != "/") {
                prompts_.at(p.path().section('/', -1)).run({});
                if (prompts_.contains(p.path().section('/', -1)))
                    wait.exec();
            }
        } else {
            unlock_quietly(kLogin);
        }
        migrate(owner);
    }
    // Ours once whoever has it goes; it's told to.
    const auto reply = dbus->registerService("org.freedesktop.secrets", QDBusConnectionInterface::QueueService,
                                             QDBusConnectionInterface::DontAllowReplacement);
    if (reply.isValid() && reply.value() == QDBusConnectionInterface::ServiceQueued && !owner.isEmpty()) {
        const QDBusReply<uint> pid = dbus->servicePid(owner);
        QFile comm(QString("/proc/%1/comm").arg(pid.value()));
        const QString name = comm.open(QIODevice::ReadOnly) ? QString::fromUtf8(comm.readAll()).trimmed() : QString();
        if (pid.isValid() && (name == "ksecretd" || name.startsWith("kwalletd") || name.startsWith("gnome-keyring"))) {
            qInfo("atrium-keyring: taking over from %s", qPrintable(name));
            ::kill(pid_t(pid.value()), SIGTERM);
        } else {
            qWarning("atrium-keyring: %s keeps org.freedesktop.secrets; waiting for it to go", qPrintable(name));
        }
    }
    return reply.isValid();
}

} // namespace atrium::keyring
