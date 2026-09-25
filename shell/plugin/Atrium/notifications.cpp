#include "notifications.hpp"

#include "compositor.hpp"

#include <QFileSystemWatcher>
#include <QGuiApplication>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace atrium {

NotificationHistory* NotificationHistory::instance() {
    static auto* self = new NotificationHistory;
    return self;
}

namespace {

constexpr int kMaxItems = 200;

QString stateFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.local/state";
    return dir + "/atrium/notifications.json";
}

} // namespace

NotificationHistory::NotificationHistory(QObject* parent) : QObject(parent), file_(stateFile()) {
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(500);
    connect(&saveTimer_, &QTimer::timeout, this, &NotificationHistory::save);
    load();
    connect(Compositor::instance(), &Compositor::settingsChanged, this, &NotificationHistory::appsChanged);
    // Elsewhere than the shell (System Settings), what the shell writes shows up too.
    if (QGuiApplication::desktopFileName() != "atrium-shell") {
        watcher_.addPath(QFileInfo(file_).path());
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] {
            load();
            emit changed();
            emit appsChanged();
        });
    }
}

NotificationHistory::~NotificationHistory() {
    if (saveTimer_.isActive())
        save();
}

void NotificationHistory::load() {
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    items_ = doc.value("items").toArray().toVariantList();
    seen_ = doc.value("apps").toObject().toVariantMap();
    for (const QVariant& v : items_)
        if (!seen_.contains(v.toMap().value("app").toString()))
            seen_.insert(v.toMap().value("app").toString(), v.toMap().value("icon"));
    for (const QVariant& v : items_)
        nextUid_ = std::max(nextUid_, v.toMap().value("uid").toInt() + 1);
}

void NotificationHistory::save() const {
    QDir().mkpath(QFileInfo(file_).path());
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(QJsonObject{{"items", QJsonArray::fromVariantList(items_)},
                                         {"apps", QJsonObject::fromVariantMap(seen_)}}).toJson(QJsonDocument::Compact));
    f.commit();
}

void NotificationHistory::scheduleSave() {
    saveTimer_.start();
}

QString NotificationHistory::modeOf(const QString& app) {
    const QVariantMap s = Compositor::instance()->settings();
    if (s.value("notifications.off").toStringList().contains(app))
        return QStringLiteral("off");
    if (s.value("notifications.quiet").toStringList().contains(app))
        return QStringLiteral("quiet");
    return QStringLiteral("on");
}

QVariantList NotificationHistory::apps() const {
    QVariantMap all = seen_;
    // Named in the settings but not seen here (another computer's registry).
    for (const char* key : {"notifications.quiet", "notifications.off"})
        for (const QString& app : Compositor::instance()->settings().value(key).toStringList())
            if (!all.contains(app))
                all.insert(app, QString());
    QVariantList out;
    for (auto it = all.begin(); it != all.end(); ++it)
        if (!it.key().isEmpty())
            out.append(QVariantMap{{"name", it.key()}, {"icon", it.value()}, {"mode", modeOf(it.key())}});
    return out;  // QVariantMap keeps them by name
}

void NotificationHistory::setAppMode(const QString& app, const QString& mode) {
    Compositor* c = Compositor::instance();
    for (const QString& m : {QStringLiteral("quiet"), QStringLiteral("off")}) {
        const QString key = "notifications." + m;
        QStringList list = c->settings().value(key).toStringList();
        const bool had = list.contains(app);
        list.removeAll(app);
        if (m == mode)
            list.append(app);
        if (had != (m == mode))
            c->setSetting(key, list);
    }
}

int NotificationHistory::add(const QVariantMap& entry) {
    QVariantMap e = entry;
    if (!seen_.contains(e.value("app").toString())) {
        seen_.insert(e.value("app").toString(), e.value("icon"));
        emit appsChanged();
    }
    const int uid = nextUid_++;
    e["uid"] = uid;
    e["time"] = QDateTime::currentMSecsSinceEpoch();
    // Only files survive a restart; inline image data does not. Theme icons
    // (image://icon/name) are names, so they do.
    for (const char* key : {"image", "icon"}) {
        const QString url = e.value(key).toString();
        if (!url.isEmpty() && !url.startsWith("file:") && !url.startsWith('/') && !url.startsWith("image://icon/"))
            e[key] = QString();
    }
    items_.prepend(e);
    QList<int> gone;
    while (items_.size() > kMaxItems)
        gone.append(items_.takeLast().toMap().value("uid").toInt());
    if (!gone.isEmpty())
        emit removed(gone);
    ++unread_;
    emit changed();
    scheduleSave();
    return uid;
}

void NotificationHistory::remove(int uid) {
    const qsizetype before = items_.size();
    items_.removeIf([uid](const QVariant& v) { return v.toMap().value("uid").toInt() == uid; });
    if (items_.size() == before)
        return;
    emit removed({uid});
    unread_ = std::min<int>(unread_, int(items_.size()));
    emit changed();
    scheduleSave();
}

void NotificationHistory::clearApp(const QString& app) {
    QList<int> gone;
    items_.removeIf([&](const QVariant& v) {
        const QVariantMap m = v.toMap();
        if (m.value("app").toString() != app)
            return false;
        gone.append(m.value("uid").toInt());
        return true;
    });
    if (!gone.isEmpty())
        emit removed(gone);
    unread_ = std::min<int>(unread_, int(items_.size()));
    emit changed();
    scheduleSave();
}

void NotificationHistory::clear() {
    if (items_.isEmpty() && !unread_)
        return;
    QList<int> gone;
    for (const QVariant& v : std::as_const(items_))
        gone.append(v.toMap().value("uid").toInt());
    items_.clear();
    unread_ = 0;
    if (!gone.isEmpty())
        emit removed(gone);
    emit changed();
    scheduleSave();
}

void NotificationHistory::markRead() {
    if (!unread_)
        return;
    unread_ = 0;
    emit changed();
}

QVariantList NotificationHistory::groups() const {
    QVariantList out;
    QHash<QString, qsizetype> index;
    for (const QVariant& v : items_) {
        const QVariantMap e = v.toMap();
        const QString app = e.value("app").toString();
        auto it = index.find(app);
        if (it == index.end()) {
            index.insert(app, out.size());
            out.push_back(QVariantMap{{"app", app}, {"icon", e.value("icon")}, {"items", QVariantList{v}}});
        } else {
            QVariantMap g = out[*it].toMap();
            QVariantList list = g.value("items").toList();
            list.push_back(v);
            g["items"] = list;
            out[*it] = g;
        }
    }
    return out;
}

QString NotificationHistory::ago(qint64 ms) const {
    const QDateTime then = QDateTime::fromMSecsSinceEpoch(ms);
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 secs = then.secsTo(now);
    if (secs < 60)
        return "now";
    if (secs < 3600)
        return QString::number(secs / 60) + "m";
    if (then.date() == now.date())
        return QString::number(secs / 3600) + "h";
    if (then.date() == now.date().addDays(-1))
        return "Yesterday";
    return then.toString("ddd d MMM");
}

} // namespace atrium
