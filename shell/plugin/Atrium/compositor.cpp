#include "compositor.hpp"

#include "accent.hpp"
#include "desktop_entries.hpp"
#include "palette.hpp"

#include <QDateTime>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>

#include <algorithm>

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
    // Apps that run in a terminal get the one the shortcut opens.
    connect(this, &Compositor::settingsChanged, this, [this] {
        shell::DesktopEntries::instance()->setTerminal(settings_.value("shortcuts.terminal").toString());
        // The shell's own icons in the theme picked for apps.
        const QString icons = settings_.value("appearance.icon_theme").toString();
        if (!icons.isEmpty() && icons != QIcon::themeName())
            QIcon::setThemeName(icons);
    });
    connect(&requests_, &QLocalSocket::connected, this, [this] {
        emit connectedChanged();
        refreshAll();
    });
    connect(&events_, &QLocalSocket::connected, this, [this] {
        const QJsonObject sub{{"cmd", "subscribe"},
                              {"topics", QJsonArray{"windows", "spaces", "outputs", "settings", "shell", "keyboard", "portal", "night_light"}}};
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

void Compositor::requestFull(QJsonObject req, FullReply reply) {
    if (requests_.state() != QLocalSocket::ConnectedState)
        return;
    const qint64 id = nextId_++;
    req["id"] = id;
    fullPending_[id] = std::move(reply);
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
    request({{"cmd", "keyboard"}}, [this](const QJsonValue& r) {
        keyboard_ = r.toObject().toVariantMap();
        emit keyboardChanged();
    });
    request({{"cmd", "night_light"}}, [this](const QJsonValue& r) { takeNightLight(r.toObject()); });
    request({{"cmd", "settings.get"}}, [this](const QJsonValue& r) {
        settings_ = r.toObject().toVariantMap();
        emit settingsChanged();
    });
    request({{"cmd", "settings.schema"}}, [this](const QJsonValue& r) {
        schema_ = r.toArray().toVariantList();
        emit schemaChanged();
    });
    request({{"cmd", "actions"}}, [this](const QJsonValue& r) {
        actions_.clear();
        for (const QJsonValue& v : r.toArray())
            actions_.push_back(v.toString());
        emit shortcutsChanged();
    });
    for (const char* table : {"apps", "rules", "shortcuts"})
        refreshTable(table);
}

void Compositor::readReplies() {
    for (const QByteArray& line : takeLines(requests_, requestBuffer_)) {
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        if (auto full = fullPending_.find(reply.value("id").toInteger(-1)); full != fullPending_.end()) {
            FullReply done = std::move(full->second);
            fullPending_.erase(full);
            done(reply);
            continue;
        }
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
    } else if (kind == "registry.changed") {
        refreshTable(e.value("table").toString());
    } else if (kind == "shortcut.activated" || kind == "shortcut.deactivated") {
        emit portalShortcut(e.value("app").toString(), e.value("id").toString(), kind == "shortcut.activated");
    } else if (kind == "night_light.changed") {
        takeNightLight(e.value("night_light").toObject());
    } else if (kind == "keyboard.changed") {
        keyboard_ = e.value("keyboard").toObject().toVariantMap();
        emit keyboardChanged();
    } else if (kind == "setting.changed") {
        settings_[e.value("key").toString()] = e.value("value").toVariant();
        emit settingsChanged();
    } else if (kind == "text.not_inserted") {
        emit textNotInserted(e.value("text").toString());
    } else if (kind == "window.menu") {
        emit windowMenu(e.value("window").toObject().toVariantMap(), e.value("output").toString(),
                        e.value("x").toInt(), e.value("y").toInt());
    } else if (kind.startsWith("window.")) {
        const QVariantMap w = e.value("window").toObject().toVariantMap();
        if (kind == "window.closed") {
            const int id = w.value("id").toInt();
            windows_.removeIf([id](const QVariant& v) { return v.toMap().value("id").toInt() == id; });
        } else {
            upsertWindow(w, kind == "window.focused" || kind == "window.opened");
        }
        emit windowsChanged();
    } else if (kind == "pointer.pressed") {
        emit pointerPressed(e.value("namespace").toString());
    } else if (kind == "output.edge") {
        emit edgeReached(e.value("output").toString(), e.value("edge").toString());
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
        if (!w.value("secret").toBool() && !w.value("skip_taskbar").toBool() &&
            w.value("output").toString() == output && w.value("space").toString() == label)
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

void Compositor::windowRequest(int id, const QString& command, const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "window." + command;
    req["window"] = id;
    change(req);
}

void Compositor::insertText(const QString& text) {
    change({{"cmd", "text.insert"}, {"text", text}});
}

void Compositor::closeWindow(int id) {
    request({{"cmd", "window.close"}, {"window", id}});
}

QVariantMap Compositor::palette(bool light, const QString& accent) const {
    const palette::Palette p = palette::make(light, accent.toStdString());
    auto c = [](uint32_t v) { return QString::fromStdString(palette::hex(v)); };
    return {{"label", c(p.label)},
            {"secondaryLabel", c(p.secondary_label)},
            {"tertiaryLabel", c(p.tertiary_label)},
            {"quaternaryLabel", c(p.quaternary_label)},
            {"fill", c(p.fill)},
            {"secondaryFill", c(p.secondary_fill)},
            {"tertiaryFill", c(p.tertiary_fill)},
            {"quaternaryFill", c(p.quaternary_fill)},
            {"separator", c(p.separator)},
            {"windowBackground", c(p.window_background)},
            {"controlBackground", c(p.control_background)},
            {"control", c(p.control)},
            {"thumb", c(p.thumb)},
            {"groupedBackground", c(p.grouped_background)},
            {"accent", c(p.accent)},
            {"labelOnAccent", c(p.on_accent)},
            {"accentFill", c(p.accent_fill)},
            {"focusRing", c(p.focus_ring)},
            {"red", c(p.red)},
            {"orange", c(p.orange)},
            {"yellow", c(p.yellow)},
            {"green", c(p.green)},
            {"mint", c(p.mint)},
            {"teal", c(p.teal)},
            {"cyan", c(p.cyan)},
            {"blue", c(p.blue)},
            {"indigo", c(p.indigo)},
            {"purple", c(p.purple)},
            {"pink", c(p.pink)},
            {"brown", c(p.brown)},
            {"gray", c(p.gray)},
            {"shadow", c(p.shadow)},
            {"scrim", c(p.scrim)}};
}

QString Compositor::accentColor(const QString& name) const {
    const auto rgb = accent::seed(name.toStdString());
    return rgb ? QString::asprintf("#%06x", *rgb) : QString();
}

QString Compositor::accentLabel(const QString& name) const {
    const std::string_view l = accent::label(name.toStdString());
    return QString::fromUtf8(l.data(), qsizetype(l.size()));
}

void Compositor::action(const QString& name, const QVariant& arg) {
    QJsonObject req{{"cmd", "action"}, {"name", name}};
    if (arg.isValid() && !arg.isNull())
        req["arg"] = arg.toString();
    request(req);
}

void Compositor::takeNightLight(const QJsonObject& state) {
    nightLight_ = state.toVariantMap();
    // "On until 7:00 AM", in the clock's own style.
    const qint64 until = state.value("until").toInteger();
    QString note;
    if (until > 0) {
        const QTime at = QDateTime::fromSecsSinceEpoch(until).time();
        const bool h24 = settings_.value("clock.24_hour").toBool();
        note = QString(state.value("active").toBool() ? "On until " : "Off until ") +
               QLocale().toString(at, h24 ? QStringLiteral("HH:mm") : QStringLiteral("h:mm AP"));
    }
    nightLight_["note"] = note;
    emit nightLightChanged();
}

void Compositor::setNightLight(bool on) {
    request({{"cmd", "night_light.set"}, {"active", on}});
}

void Compositor::setKeyboardLayout(int index) {
    request({{"cmd", "keyboard.layout"}, {"index", index}});
}

void Compositor::setSetting(const QString& key, const QVariant& value) {
    request({{"cmd", "settings.set"}, {"key", key}, {"value", QJsonValue::fromVariant(value)}});
}

void Compositor::resetSetting(const QString& key) {
    request({{"cmd", "settings.reset"}, {"key", key}});
}

// --- registry ------------------------------------------------------------------------

void Compositor::refreshTable(const QString& table) {
    const QString cmd = table + ".list";
    request({{"cmd", cmd}}, [this, table](const QJsonValue& r) {
        const QVariantList list = r.toArray().toVariantList();
        if (table == "apps") {
            apps_ = list;
            emit appsChanged();
        } else if (table == "rules") {
            rules_ = list;
            emit rulesChanged();
        } else if (table == "shortcuts") {
            shortcuts_ = list;
            emit shortcutsChanged();
        }
    });
}

QStringList Compositor::dockPins() const {
    std::vector<std::pair<int, QString>> pins;
    for (const QVariant& v : apps_) {
        const QVariantMap a = v.toMap();
        if (a.value("dock").isValid() && !a.value("dock").isNull())
            pins.emplace_back(a.value("dock").toInt(), a.value("app_id").toString());
    }
    std::ranges::sort(pins);
    QStringList out;
    for (const auto& [_, id] : pins)
        out.push_back(id);
    return out;
}

// A registry request whose refusal the Settings app shows.
void Compositor::change(QJsonObject req) {
    auto* self = this;
    requestFull(std::move(req), [self](const QJsonObject& reply) {
        if (!reply.value("ok").toBool())
            emit self->refused(reply.value("error").toString());
    });
}

void Compositor::refreshDevices() {
    request({{"cmd", "devices"}}, [this](const QJsonValue& r) {
        devices_ = r.toArray().toVariantList();
        emit devicesChanged();
    });
}

void Compositor::setDevice(const QString& name, const QVariantMap& fields) {
    QJsonObject req{{"cmd", "device.set"}, {"device", name}};
    for (auto it = fields.begin(); it != fields.end(); ++it)
        req[it.key()] = it.value().isValid() && !it.value().isNull() ? QJsonValue::fromVariant(it.value())
                                                                     : QJsonValue(QJsonValue::Null);
    requestFull(req, [this](const QJsonObject& reply) {
        if (!reply.value("ok").toBool()) {
            emit refused(reply.value("error").toString());
            return;
        }
        devices_ = reply.value("result").toArray().toVariantList();
        emit devicesChanged();
    });
}

void Compositor::setApp(const QString& appId, const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "app.set";
    req["app_id"] = appId;
    change(req);
}

void Compositor::forgetApp(const QString& appId) {
    change({{"cmd", "app.remove"}, {"app_id", appId}});
}

void Compositor::setDock(const QStringList& appIds) {
    change({{"cmd", "dock.set"}, {"apps", QJsonArray::fromStringList(appIds)}});
}

void Compositor::setPinned(const QString& appId, bool pinned) {
    QStringList pins = dockPins();
    if (pinned == pins.contains(appId))
        return;
    if (pinned)
        pins.push_back(appId);
    else
        pins.removeAll(appId);
    setDock(pins);
}

void Compositor::configureOutput(const QString& name, const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "output.set";
    req["output"] = name;
    change(req);
}

void Compositor::addRule(const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "rule.add";
    change(req);
}

void Compositor::setRule(qint64 id, const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "rule.set";
    req["rule"] = id;
    change(req);
}

void Compositor::removeRule(qint64 id) {
    change({{"cmd", "rule.remove"}, {"rule", id}});
}

void Compositor::addShortcut(const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "shortcut.add";
    change(req);
}

void Compositor::setShortcut(qint64 id, const QVariantMap& fields) {
    QJsonObject req = QJsonObject::fromVariantMap(fields);
    req["cmd"] = "shortcut.set";
    req["shortcut"] = id;
    change(req);
}

void Compositor::removeShortcut(qint64 id) {
    change({{"cmd", "shortcut.remove"}, {"shortcut", id}});
}

void Compositor::resetShortcuts() {
    change({{"cmd", "shortcuts.reset"}});
}

} // namespace atrium
