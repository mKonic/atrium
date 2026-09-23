#include "compositor.hpp"

#include <QJsonArray>
#include <QJsonDocument>

namespace atrium {

namespace {

QString socketPath() {
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    const QByteArray display = qgetenv("WAYLAND_DISPLAY");
    return QString::fromLocal8Bit(runtime) + "/atrium." + QString::fromLocal8Bit(display) + ".sock";
}

// Lines complete so far; what is left of an unfinished one stays in `buffer`.
QList<QByteArray> takeLines(QLocalSocket& socket, QByteArray& buffer) {
    buffer += socket.readAll();
    QList<QByteArray> lines;
    qsizetype nl;
    while ((nl = buffer.indexOf('\n')) >= 0) {
        lines.push_back(buffer.left(nl));
        buffer.remove(0, nl + 1);
    }
    return lines;
}

} // namespace

Compositor* Compositor::instance() {
    static Compositor* self = new Compositor;
    return self;
}

Compositor::Compositor(QObject* parent) : QObject(parent), path_(socketPath()) {
    connect(&requests_, &QLocalSocket::readyRead, this, &Compositor::readReplies);
    connect(&events_, &QLocalSocket::readyRead, this, &Compositor::readEvents);
    connect(&requests_, &QLocalSocket::connected, this, [this] {
        emit connectedChanged();
        refreshAll();
    });
    connect(&events_, &QLocalSocket::connected, this, [this] {
        const QJsonObject sub{{"cmd", "subscribe"},
                              {"topics", QJsonArray{"windows", "spaces", "outputs", "settings", "shell"}}};
        events_.write(QJsonDocument(sub).toJson(QJsonDocument::Compact) + '\n');
        emit connectedChanged();
    });
    for (QLocalSocket* s : {&requests_, &events_})
        connect(s, &QLocalSocket::disconnected, this, [this] {
            pending_.clear();
            emit connectedChanged();
            reconnect_.start();
        });

    // The compositor restarted or the shell came up first: keep trying.
    reconnect_.setInterval(1000);
    connect(&reconnect_, &QTimer::timeout, this, &Compositor::connectBoth);
    connectBoth();
}

bool Compositor::connected() const {
    return requests_.state() == QLocalSocket::ConnectedState && events_.state() == QLocalSocket::ConnectedState;
}

void Compositor::connectBoth() {
    for (QLocalSocket* s : {&requests_, &events_})
        if (s->state() == QLocalSocket::UnconnectedState)
            s->connectToServer(path_);
    if (connected())
        reconnect_.stop();
    else
        reconnect_.start();
}

void Compositor::request(QJsonObject req, Reply reply) {
    if (requests_.state() != QLocalSocket::ConnectedState)
        return;
    const qint64 id = nextId_++;
    req["id"] = id;
    if (reply)
        pending_[id] = std::move(reply);
    requests_.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
}

void Compositor::refreshAll() {
    request({{"cmd", "windows"}}, [this](const QJsonValue& r) {
        windows_ = r.toArray().toVariantList();
        emit windowsChanged();
    });
    request({{"cmd", "spaces"}}, [this](const QJsonValue& r) {
        spaces_ = r.toArray().toVariantList();
        emit spacesChanged();
    });
    request({{"cmd", "outputs"}}, [this](const QJsonValue& r) {
        outputs_ = r.toArray().toVariantList();
        emit outputsChanged();
    });
    request({{"cmd", "settings.get"}}, [this](const QJsonValue& r) {
        settings_ = r.toObject().toVariantMap();
        emit settingsChanged();
    });
    request({{"cmd", "settings.schema"}}, [this](const QJsonValue& r) {
        schema_ = r.toArray().toVariantList();
        emit schemaChanged();
    });
}

void Compositor::readReplies() {
    for (const QByteArray& line : takeLines(requests_, requestBuffer_)) {
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        auto it = pending_.find(reply.value("id").toInteger(-1));
        if (it == pending_.end())
            continue;
        Reply done = std::move(it->second);
        pending_.erase(it);
        if (reply.value("ok").toBool())
            done(reply.value("result"));
    }
}

void Compositor::readEvents() {
    for (const QByteArray& line : takeLines(events_, eventBuffer_))
        applyEvent(QJsonDocument::fromJson(line).object());
}

void Compositor::applyEvent(const QJsonObject& e) {
    const QString kind = e.value("event").toString();
    if (kind == "shell.action") {
        emit shellAction(e.value("name").toString());
    } else if (kind == "spaces.changed") {
        spaces_ = e.value("spaces").toArray().toVariantList();
        emit spacesChanged();
    } else if (kind == "setting.changed") {
        settings_[e.value("key").toString()] = e.value("value").toVariant();
        emit settingsChanged();
    } else if (kind.startsWith("window.")) {
        const QVariantMap w = e.value("window").toObject().toVariantMap();
        if (kind == "window.closed") {
            const int id = w.value("id").toInt();
            windows_.removeIf([id](const QVariant& v) { return v.toMap().value("id").toInt() == id; });
        } else {
            upsertWindow(w, kind == "window.focused" || kind == "window.opened");
        }
        emit windowsChanged();
    } else if (kind == "outputs.changed") {
        request({{"cmd", "outputs"}}, [this](const QJsonValue& r) {
            outputs_ = r.toArray().toVariantList();
            emit outputsChanged();
        });
    }
}

void Compositor::upsertWindow(const QVariantMap& w, bool toFront) {
    const int id = w.value("id").toInt();
    const bool focused = w.value("focused").toBool();
    qsizetype at = -1;
    for (qsizetype i = 0; i < windows_.size(); ++i) {
        QVariantMap m = windows_[i].toMap();
        if (m.value("id").toInt() == id) {
            at = i;
        } else if (focused && m.value("focused").toBool()) {
            m["focused"] = false;  // only one window has focus
            windows_[i] = m;
        }
    }
    if (at >= 0)
        windows_.removeAt(at);
    if (toFront || at < 0)
        windows_.prepend(w);
    else
        windows_.insert(at, w);
}

// --- lookups ---------------------------------------------------------------------------

QVariant Compositor::focusedWindow() const {
    for (const QVariant& v : windows_)
        if (v.toMap().value("focused").toBool())
            return v;
    return {};
}

QVariant Compositor::focusedOutput() const {
    for (const QVariant& v : outputs_)
        if (v.toMap().value("focused").toBool())
            return v;
    return outputs_.isEmpty() ? QVariant() : outputs_.first();
}

QVariant Compositor::shownSecret() const {
    for (const QVariant& v : spaces_) {
        const QVariantMap s = v.toMap();
        if (s.value("secret").toBool() && s.value("shown").toBool())
            return v;
    }
    return {};
}

int Compositor::activeSpace(const QString& output) const {
    for (const QVariant& v : spaces_) {
        const QVariantMap s = v.toMap();
        if (!s.value("secret").toBool() && s.value("shown").toBool() && s.value("output").toString() == output)
            return s.value("number").toInt();
    }
    return 1;
}

QVariantList Compositor::spacesOn(const QString& output) const {
    QVariantList out;
    for (const QVariant& v : spaces_) {
        const QVariantMap s = v.toMap();
        if (!s.value("secret").toBool() && s.value("output").toString() == output)
            out.push_back(v);
    }
    return out;
}

QVariantList Compositor::windowsOn(const QString& output, int space) const {
    const QString label = QString::number(space);
    QVariantList out;
    for (const QVariant& v : windows_) {
        const QVariantMap w = v.toMap();
        if (!w.value("secret").toBool() && w.value("output").toString() == output && w.value("space").toString() == label)
            out.push_back(v);
    }
    return out;
}

bool Compositor::fullscreenOn(const QString& output) const {
    const QString label = QString::number(activeSpace(output));
    for (const QVariant& v : windows_) {
        const QVariantMap w = v.toMap();
        if (w.value("fullscreen").toBool() && !w.value("minimized").toBool() && !w.value("secret").toBool() &&
            w.value("output").toString() == output && w.value("space").toString() == label)
            return true;
    }
    return false;
}

QVariant Compositor::setting(const QString& key, const QVariant& fallback) const {
    auto it = settings_.find(key);
    return it == settings_.end() || !it->isValid() ? fallback : *it;
}

// --- requests ----------------------------------------------------------------------------

void Compositor::switchSpace(int number) {
    request({{"cmd", "space.switch"}, {"number", number}});
}

void Compositor::toggleSecret(const QString& name) {
    request({{"cmd", "secret.toggle"}, {"name", name}});
}

void Compositor::focusWindow(int id) {
    request({{"cmd", "window.focus"}, {"window", id}});
}

void Compositor::closeWindow(int id) {
    request({{"cmd", "window.close"}, {"window", id}});
}

void Compositor::action(const QString& name, const QVariant& arg) {
    QJsonObject req{{"cmd", "action"}, {"name", name}};
    if (arg.isValid() && !arg.isNull())
        req["arg"] = arg.toString();
    request(req);
}

void Compositor::setSetting(const QString& key, const QVariant& value) {
    request({{"cmd", "settings.set"}, {"key", key}, {"value", QJsonValue::fromVariant(value)}});
}

void Compositor::resetSetting(const QString& key) {
    request({{"cmd", "settings.reset"}, {"key", key}});
}

} // namespace atrium
