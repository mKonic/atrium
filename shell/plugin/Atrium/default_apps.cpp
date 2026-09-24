#include "default_apps.hpp"

#include "compositor.hpp"
#include "desktop_entries.hpp"
#include "ini_core.hpp"
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

struct Kind {
    const char* kind;
    const char* title;
    QStringList types;  // the first decides which apps are offered, and which is current
};

const QList<Kind>& kindList() {
    static const QList<Kind> list = {
        {"browser", "Web browser",
         {"x-scheme-handler/https", "x-scheme-handler/http", "text/html", "application/xhtml+xml"}},
        {"mail", "Email", {"x-scheme-handler/mailto"}},
        {"files", "Files", {"inode/directory"}},
        {"text", "Text editor", {"text/plain"}},
        {"images", "Images", {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/avif",
                              "image/jxl", "image/svg+xml", "image/tiff"}},
        {"video", "Video", {"video/mp4", "video/x-matroska", "video/webm", "video/quicktime",
                            "video/x-msvideo", "video/mpeg"}},
        {"music", "Music", {"audio/mpeg", "audio/flac", "audio/ogg", "audio/x-wav", "audio/aac", "audio/mp4",
                            "audio/opus"}},
        {"pdf", "PDF documents", {"application/pdf"}},
    };
    return list;
}

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

// The default for the first of `types` that has one, most important file
// first; with none set, the first app installed for it, as xdg-open picks.
static QString currentFor(const QStringList& types) {
    auto first = [](const std::string& list) {
        QString id = QString::fromStdString(list.substr(0, list.find(';')));
        id.chop(id.endsWith(".desktop") ? 8 : 0);
        return id;
    };
    for (const QString& file : mimeappsFiles()) {
        const std::string text = read(file).toStdString();
        for (const QString& type : types) {
            const std::string id = mimeapps::default_for(text, type.toStdString());
            if (!id.empty())
                return first(id);
        }
    }
    for (const QString& dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        const std::string cache = read(dir + "/applications/mimeinfo.cache").toStdString();
        for (const QString& type : types)
            if (auto apps = ini::get(cache, "MIME Cache", type.toStdString()); apps && !apps->empty())
                return first(*apps);
    }
    return {};
}

static QVariantList appsFor(const QString& type) {
    return entries([&](DesktopEntry* e) { return e->mimeTypes().contains(type); });
}

QString DefaultApps::browser() const {
    return currentFor(kindList().first().types);
}

QVariantList DefaultApps::kinds() const {
    QVariantList out;
    for (const Kind& k : kindList()) {
        QVariantList apps = appsFor(k.types.first());
        const QString current = currentFor(k.types);
        // One that isn't offered (hidden, or it opens another of the kind's
        // types) is still listed by its name, being the one in use.
        const bool listed = std::ranges::any_of(apps, [&](const QVariant& a) { return a.toMap().value("value") == current; });
        if (!listed)
            if (DesktopEntry* e = DesktopEntries::instance()->byId(current))
                apps.prepend(QVariantMap{{"value", e->id()}, {"label", e->name()}, {"icon", e->icon()}});
        out.append(QVariantMap{{"kind", k.kind}, {"title", k.title}, {"apps", apps}, {"current", current}});
    }
    return out;
}

void DefaultApps::setDefault(const QString& kind, const QString& id) {
    for (const Kind& k : kindList()) {
        if (kind != k.kind)
            continue;
        // Every type of the kind the app opens (all of them for a browser,
        // whose list is the scheme handlers and pages).
        DesktopEntry* e = DesktopEntries::instance()->byId(id);
        const QStringList opens = e ? e->mimeTypes() : QStringList();
        const QString file = userFile("mimeapps.list");
        std::string text = read(file).toStdString();
        for (const QString& type : k.types)
            if (kind == "browser" || opens.contains(type))
                text = mimeapps::set_default(text, type.toStdString(), (id + ".desktop").toStdString());
        write(file, QByteArray::fromStdString(text));
        emit changed();
        return;
    }
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
