#include "default_apps.hpp"

#include "compositor.hpp"
#include "desktop_entries.hpp"
#include "mimeapps_core.hpp"
#include "terminal.hpp"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

namespace atrium {

namespace {

using shell::DesktopEntries;
using shell::DesktopEntry;

const QStringList kWebTypes = {"x-scheme-handler/http", "x-scheme-handler/https", "text/html",
                               "application/xhtml+xml"};

QString userFile(const QString& name) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/" + name;
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

// The apps shown on this desktop that `wanted` picks, by name.
QVariantList entries(const std::function<bool(DesktopEntry*)>& wanted) {
    QList<DesktopEntry*> found;
    for (QObject* o : DesktopEntries::instance()->applications()->values()) {
        auto* e = qobject_cast<DesktopEntry*>(o);
        if (e && !e->noDisplay() && wanted(e))
            found.append(e);
    }
    QCollator order;
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(found.begin(), found.end(),
              [&](DesktopEntry* a, DesktopEntry* b) { return order.compare(a->name(), b->name()) < 0; });
    QVariantList out;
    for (DesktopEntry* e : found)
        out.append(QVariantMap{{"value", e->id()}, {"label", e->name()}, {"icon", e->icon()}});
    return out;
}

// Where mimeapps.list files are looked for, most important first.
QStringList mimeappsFiles() {
    QStringList files = {userFile("mimeapps.list")};
    for (const QString& dir : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation).mid(1))
        files.append(dir + "/mimeapps.list");
    for (const QString& dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        files.append(dir + "/applications/mimeapps.list");
    return files;
}

QString program(DesktopEntry* e) {
    const QStringList argv = e ? e->command() : QStringList();
    return argv.isEmpty() ? QString() : QFileInfo(argv.first()).fileName();
}

} // namespace

DefaultApps::DefaultApps(QObject* parent) : QObject(parent) {
    connect(DesktopEntries::instance(), &DesktopEntries::applicationsChanged, this, &DefaultApps::changed);
    connect(Compositor::instance(), &Compositor::settingsChanged, this, &DefaultApps::changed);
}

QVariantList DefaultApps::browsers() const {
    return entries([](DesktopEntry* e) {
        const QStringList types = e->mimeTypes();
        return types.contains("x-scheme-handler/https") || types.contains("x-scheme-handler/http");
    });
}

QString DefaultApps::browser() const {
    for (const QString& file : mimeappsFiles()) {
        const std::string text = read(file).toStdString();
        for (const char* type : {"x-scheme-handler/https", "x-scheme-handler/http", "text/html"}) {
            QString id = QString::fromStdString(mimeapps::default_for(text, type));
            if (!id.isEmpty()) {
                id.chop(id.endsWith(".desktop") ? 8 : 0);
                return id;
            }
        }
    }
    return {};
}

void DefaultApps::setBrowser(const QString& id) {
    const QString file = userFile("mimeapps.list");
    std::string text = read(file).toStdString();
    for (const QString& type : kWebTypes)
        text = mimeapps::set_default(text, type.toStdString(), (id + ".desktop").toStdString());
    write(file, QByteArray::fromStdString(text));
    emit changed();
}

QVariantList DefaultApps::terminals() const {
    return entries([](DesktopEntry* e) { return e->categories().contains("TerminalEmulator"); });
}

QString DefaultApps::terminal() const {
    // The one the setting names, else the one atrium would pick itself.
    QStringList order;
    const QString set = Compositor::instance()->setting("shortcuts.terminal", QString()).toString();
    if (!set.isEmpty()) {
        order.append(QFileInfo(set.section(' ', 0, 0)).fileName());
    } else {
        for (const Terminal& t : kTerminals)
            if (!QStandardPaths::findExecutable(t.program).isEmpty())
                order.append(t.program);
    }
    const QVariantList all = terminals();
    for (const QString& name : order)
        for (const QVariant& v : all) {
            const QString id = v.toMap().value("value").toString();
            if (program(DesktopEntries::instance()->byId(id)) == name)
                return id;
        }
    return {};
}

void DefaultApps::setTerminal(const QString& id) {
    const QString prog = program(DesktopEntries::instance()->byId(id));
    if (prog.isEmpty())
        return;
    Compositor::instance()->setSetting("shortcuts.terminal", prog);
    // xdg-terminal-exec's own choice: this one first, the rest kept.
    const QString list = userFile("xdg-terminals.list");
    QStringList lines = QString::fromUtf8(read(list)).split('\n', Qt::SkipEmptyParts);
    lines.removeAll(id + ".desktop");
    lines.prepend(id + ".desktop");
    write(list, (lines.join('\n') + "\n").toUtf8());
    emit changed();
}

} // namespace atrium
