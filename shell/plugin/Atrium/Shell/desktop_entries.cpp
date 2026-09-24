#include "desktop_entries.hpp"
#include "terminal.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace atrium::shell {

namespace {

QStringList strings(const std::vector<std::string>& v) {
    QStringList out;
    for (const std::string& s : v)
        out.append(QString::fromStdString(s));
    return out;
}

std::string messages_locale() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const QByteArray v = qgetenv(var); !v.isEmpty())
            return v.toStdString();
    return {};
}

bool installed(const std::string& try_exec) {
    if (try_exec.empty())
        return true;
    const QString t = QString::fromStdString(try_exec);
    if (t.startsWith('/'))
        return QFileInfo(t).isExecutable();
    return !QStandardPaths::findExecutable(t).isEmpty();
}

} // namespace

// --- DesktopAction -----------------------------------------------------------

DesktopAction::DesktopAction(const desktop_entry::Action& a, DesktopEntry* entry)
    : QObject(entry), a_(a), entry_(entry) {}

QStringList DesktopAction::command() const {
    return entry_->argv(a_.exec);
}

void DesktopAction::execute() const {
    entry_->launch(command());
}

// --- DesktopEntry ------------------------------------------------------------

DesktopEntry::DesktopEntry(QString id, QString file, desktop_entry::Entry e, bool shown, QObject* parent)
    : QObject(parent), id_(std::move(id)), file_(std::move(file)), e_(std::move(e)), shown_(shown) {
    for (const desktop_entry::Action& a : e_.actions)
        actions_.append(new DesktopAction(a, this));
}

QStringList DesktopEntry::keywords() const {
    return strings(e_.keywords);
}

QStringList DesktopEntry::categories() const {
    return strings(e_.categories);
}

QStringList DesktopEntry::mimeTypes() const {
    return strings(e_.mime_types);
}

QStringList DesktopEntry::argv(const std::string& exec) const {
    return strings(desktop_entry::exec_argv(exec, e_.name, e_.icon, file_.toStdString()));
}

void DesktopEntry::execute() const {
    launch(command());
}

void DesktopEntry::launch(QStringList argv) const {
    if (argv.isEmpty())
        return;
    if (e_.terminal) {
        if (!QStandardPaths::findExecutable("xdg-terminal-exec").isEmpty()) {
            argv.prepend("xdg-terminal-exec");
        } else {
            QStringList term = QProcess::splitCommand(DesktopEntries::instance()->terminal());
            if (term.isEmpty())
                for (const Terminal& t : kTerminals)
                    if (!QStandardPaths::findExecutable(t.program).isEmpty()) {
                        term = {QString::fromUtf8(t.program)};
                        break;
                    }
            if (term.isEmpty())
                return;
            term += QProcess::splitCommand(QString::fromStdString(terminal_run_args(term.first().toStdString())));
            argv = term + argv;
        }
    }
    QProcess p;
    p.setProgram(argv.takeFirst());
    p.setArguments(argv);
    if (!e_.path.empty())
        p.setWorkingDirectory(QString::fromStdString(e_.path));
    else
        p.setWorkingDirectory(QDir::homePath());
    p.startDetached();
}

// --- DesktopEntries ----------------------------------------------------------

DesktopEntries* DesktopEntries::instance() {
    static auto* self = new DesktopEntries;
    return self;
}

DesktopEntries::DesktopEntries() {
    rescan_.setSingleShot(true);
    rescan_.setInterval(300);  // a package install touches many files at once
    connect(&rescan_, &QTimer::timeout, this, &DesktopEntries::scan);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &rescan_, qOverload<>(&QTimer::start));
    scan();
}

void DesktopEntries::setTerminal(const QString& t) {
    if (t == terminal_)
        return;
    terminal_ = t;
    emit terminalChanged();
}

QStringList DesktopEntries::directories() const {
    QStringList dirs;
    QString home = qEnvironmentVariable("XDG_DATA_HOME");
    if (home.isEmpty())
        home = QDir::homePath() + "/.local/share";
    dirs.append(home + "/applications");
    QString data = qEnvironmentVariable("XDG_DATA_DIRS");
    if (data.isEmpty())
        data = "/usr/local/share:/usr/share";
    for (const QString& d : data.split(':', Qt::SkipEmptyParts))
        dirs.append(d + "/applications");
    dirs.removeDuplicates();
    return dirs;
}

void DesktopEntries::scan() {
    const std::string locale = messages_locale();
    const std::string desktops = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toStdString();
    QHash<QString, DesktopEntry*> found;
    QSet<QString> taken;  // ids a higher directory already decided, Hidden ones included
    QList<QObject*> values;
    QStringList watched;

    for (const QString& root : directories()) {
        if (!QFileInfo(root).isDir())
            continue;
        watched.append(root);
        QDirIterator it(root, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            const QString path = it.next();
            // The id: the path under applications/, "/" as "-", no suffix.
            QString id = path.mid(root.size() + 1);
            id.chop(8);
            id.replace('/', '-');
            if (taken.contains(id))
                continue;
            taken.insert(id);
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QByteArray text = f.readAll();
            auto e = desktop_entry::parse(std::string_view(text.constData(), size_t(text.size())), locale);
            if (!e || e->hidden || e->type != "Application" || e->name.empty() || !installed(e->try_exec))
                continue;
            const bool shown = !e->no_display && desktop_entry::shown_in(*e, desktops);
            auto* entry = new DesktopEntry(id, path, std::move(*e), shown, this);
            found.insert(id, entry);
            values.append(entry);
        }
        QDirIterator dirs(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext())
            watched.append(dirs.next());
    }

    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    watcher_.addPaths(watched);

    const QList<QObject*> old = list_.values();
    byId_ = std::move(found);
    list_.set(values);
    emit applicationsChanged();
    // After everyone heard the new list.
    for (QObject* o : old)
        o->deleteLater();
}

DesktopEntry* DesktopEntries::byId(const QString& id) const {
    if (id.isEmpty())
        return nullptr;
    QString key = id;
    if (key.endsWith(".desktop"))
        key.chop(8);
    return byId_.value(key, nullptr);
}

DesktopEntry* DesktopEntries::heuristicLookup(const QString& name) const {
    if (name.isEmpty())
        return nullptr;
    if (DesktopEntry* e = byId(name))
        return e;
    const QString lower = name.toLower();
    // In directory order, so the user's own entries win.
    QList<DesktopEntry*> all;
    for (QObject* o : list_.values())
        all.append(static_cast<DesktopEntry*>(o));
    for (DesktopEntry* e : all)
        if (e->id().toLower() == lower)
            return e;
    for (DesktopEntry* e : all)
        if (e->startupClass().toLower() == lower)
            return e;
    for (DesktopEntry* e : all)
        if (e->id().toLower().endsWith("." + lower))
            return e;
    for (DesktopEntry* e : all)
        if (e->name().toLower() == lower)
            return e;
    return nullptr;
}

} // namespace atrium::shell
