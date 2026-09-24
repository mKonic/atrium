#include "autostart.hpp"

#include "desktop_entries.hpp"
#include "desktop_entry_core.hpp"
#include "ini_core.hpp"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace atrium {

namespace {

constexpr std::string_view kGroup = "Desktop Entry";

QString systemDir() {
    return QStringLiteral("/etc/xdg/autostart");
}

QByteArray read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).path());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    return f.commit();
}

// Off: Hidden, or GNOME's own switch.
bool turnedOff(const std::string& text) {
    return ini::get(text, kGroup, "Hidden") == "true" ||
           ini::get(text, kGroup, "X-GNOME-Autostart-enabled") == "false";
}

} // namespace

Autostart::Autostart(QObject* parent) : QObject(parent) {
    QDir().mkpath(userDir());
    watcher_.addPaths({userDir(), systemDir()});
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &Autostart::reload);
    reload();
}

QString Autostart::userDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
}

void Autostart::reload() {
    const std::string desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP", "atrium").toStdString();
    const std::string locale = QLocale().name().toStdString();
    // The user's copy of an entry wins over the system's.
    QMap<QString, QString> files;
    for (const QString& dir : {systemDir(), userDir()})
        for (const QFileInfo& fi : QDir(dir).entryInfoList({"*.desktop"}, QDir::Files))
            files[fi.completeBaseName()] = fi.filePath();
    QVariantList out;
    for (auto it = files.begin(); it != files.end(); ++it) {
        const std::string text = read(it.value()).toStdString();
        const auto e = desktop_entry::parse(text, locale);
        // What it was before the user's copy, for its name and whether it's the system's.
        const bool own = !QFile::exists(systemDir() + "/" + it.key() + ".desktop");
        const auto original = own ? e : desktop_entry::parse(read(systemDir() + "/" + it.key() + ".desktop").toStdString(), locale);
        if (!original || !desktop_entry::shown_in(*original, desktops))
            continue;
        const QString name = QString::fromStdString(e && !e->name.empty() ? e->name : original->name);
        out.append(QVariantMap{{"id", it.key()},
                               {"name", name.isEmpty() ? it.key() : name},
                               {"icon", QString::fromStdString(original->icon)},
                               {"enabled", !turnedOff(text)},
                               {"own", own}});
    }
    QCollator order;
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(out.begin(), out.end(), [&](const QVariant& a, const QVariant& b) {
        return order.compare(a.toMap().value("name").toString(), b.toMap().value("name").toString()) < 0;
    });
    entries_ = out;
    emit changed();
}

void Autostart::setEnabled(const QString& id, bool on) {
    const QString mine = userDir() + "/" + id + ".desktop";
    const QString system = systemDir() + "/" + id + ".desktop";
    const bool systemOn = QFile::exists(system) && !turnedOff(read(system).toStdString());
    // Back to the system's own, when that's already what's asked.
    if (QFile::exists(mine) && QFile::exists(system) && on == systemOn) {
        QFile::remove(mine);
    } else {
        std::string text = read(QFile::exists(mine) ? mine : system).toStdString();
        text = ini::set(text, kGroup, "Hidden", on ? std::nullopt : std::optional<std::string_view>("true"));
        if (on)
            text = ini::set(text, kGroup, "X-GNOME-Autostart-enabled", std::nullopt);
        write(mine, QByteArray::fromStdString(text));
    }
    reload();
}

QVariantList Autostart::candidates(const QString& query) const {
    QStringList here;
    for (const QVariant& v : entries_)
        here.append(v.toMap().value("id").toString());
    QList<shell::DesktopEntry*> found;
    for (QObject* o : shell::DesktopEntries::instance()->applications()->values()) {
        auto* e = qobject_cast<shell::DesktopEntry*>(o);
        if (e && !e->noDisplay() && !here.contains(e->id()) && e->name().contains(query.trimmed(), Qt::CaseInsensitive))
            found.append(e);
    }
    QCollator order;
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(found.begin(), found.end(),
              [&](auto* a, auto* b) { return order.compare(a->name(), b->name()) < 0; });
    QVariantList out;
    for (auto* e : found)
        out.append(QVariantMap{{"id", e->id()}, {"name", e->name()}, {"icon", e->icon()}});
    return out;
}

void Autostart::add(const QString& appId) {
    shell::DesktopEntry* e = shell::DesktopEntries::instance()->byId(appId);
    if (!e)
        return;
    std::string text = read(e->file()).toStdString();
    text = ini::set(text, kGroup, "Hidden", std::nullopt);
    write(userDir() + "/" + e->id() + ".desktop", QByteArray::fromStdString(text));
    reload();
}

void Autostart::remove(const QString& id) {
    if (!QFile::exists(systemDir() + "/" + id + ".desktop"))
        QFile::remove(userDir() + "/" + id + ".desktop");
    reload();
}

} // namespace atrium
