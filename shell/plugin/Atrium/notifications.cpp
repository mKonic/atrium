#include "notifications.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace atrium {

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
    for (const QVariant& v : items_)
        nextUid_ = std::max(nextUid_, v.toMap().value("uid").toInt() + 1);
}

void NotificationHistory::save() const {
    QDir().mkpath(QFileInfo(file_).path());
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(QJsonObject{{"items", QJsonArray::fromVariantList(items_)}}).toJson(QJsonDocument::Compact));
    f.commit();
}

void NotificationHistory::scheduleSave() {
    saveTimer_.start();
}

int NotificationHistory::add(const QVariantMap& entry) {
    QVariantMap e = entry;
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
    while (items_.size() > kMaxItems)
        items_.removeLast();
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
    unread_ = std::min<int>(unread_, int(items_.size()));
    emit changed();
    scheduleSave();
}

void NotificationHistory::clearApp(const QString& app) {
    items_.removeIf([&app](const QVariant& v) { return v.toMap().value("app").toString() == app; });
    unread_ = std::min<int>(unread_, int(items_.size()));
    emit changed();
    scheduleSave();
}

void NotificationHistory::clear() {
    if (items_.isEmpty() && !unread_)
        return;
    items_.clear();
    unread_ = 0;
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
