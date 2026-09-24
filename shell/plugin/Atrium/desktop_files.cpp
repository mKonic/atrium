#include "desktop_files.hpp"

#include "compositor.hpp"
#include "files.hpp"
#include "list_sync.hpp"
#include "terminal.hpp"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

namespace atrium {

namespace {

QString desktopFolder() {
    if (QString dir = qEnvironmentVariable("ATRIUM_DESKTOP_DIR"); !dir.isEmpty())
        return dir;
    // The XDG desktop folder, which need not be ~/Desktop (Qt reads user-dirs.dirs).
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (dir.isEmpty() || QDir(dir) == QDir::home())
        dir = QDir::homePath() + "/Desktop";
    return dir;
}

void run(const QString& program, const QStringList& args, const QString& cwd = {}) {
    QProcess::startDetached(program, args, cwd);
}

} // namespace

DesktopFiles::DesktopFiles(QObject* parent) : QAbstractListModel(parent), folder_(desktopFolder()) {
    // It has to exist to be watched, and for apps to put shortcuts in.
    QDir().mkpath(folder_);
    watcher_.addPath(folder_);
    settle_.setSingleShot(true);
    settle_.setInterval(60);
    connect(&settle_, &QTimer::timeout, this, &DesktopFiles::reload);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &settle_, qOverload<>(&QTimer::start));
    reload();
}

void DesktopFiles::reload() {
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    QFileInfoList infos = QDir(folder_).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    std::ranges::sort(infos, [&](const QFileInfo& a, const QFileInfo& b) {
        if (a.isDir() != b.isDir())
            return a.isDir();
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });

    std::vector<File> next;
    for (const QFileInfo& i : infos) {
        File f{.path = i.absoluteFilePath(), .name = i.fileName(), .fileName = i.fileName(), .dir = i.isDir()};
        const std::string suffix = i.suffix().toStdString();
        f.image = !f.dir && files::is_image(suffix);
        f.icon = QString::fromStdString(files::icon_for(suffix, f.dir));
        // Launchers (Steam's shortcuts) name and draw themselves.
        if (!f.dir && i.suffix() == "desktop") {
            QFile file(f.path);
            if (file.size() < 1 << 20 && file.open(QIODevice::ReadOnly)) {
                const files::Launcher l = files::parse_launcher(file.readAll().toStdString());
                if (!l.name.empty())
                    f.name = QString::fromStdString(l.name);
                f.icon = l.icon.empty() ? "application-x-executable" : QString::fromStdString(l.icon);
            }
        }
        next.push_back(std::move(f));
    }

    struct Ops {
        DesktopFiles* m;
        void insert(int i, const std::function<void()>& f) { m->beginInsertRows({}, i, i); f(); m->endInsertRows(); }
        void remove(int i, const std::function<void()>& f) { m->beginRemoveRows({}, i, i); f(); m->endRemoveRows(); }
        void move(int from, int to, const std::function<void()>& f) {
            m->beginMoveRows({}, from, from, {}, to);
            f();
            m->endMoveRows();
        }
        void change(int i, const std::function<void()>& f) {
            f();
            emit m->dataChanged(m->index(i), m->index(i));
        }
    } ops{this};
    const size_t before = files_.size();
    sync_list(files_, next, [](const File& f) { return f.path; }, ops);
    if (files_.size() != before)
        emit countChanged();

    // Whatever is gone is no longer selected.
    QStringList kept;
    for (const QString& p : selection_)
        if (std::ranges::any_of(files_, [&](const File& f) { return f.path == p; }))
            kept.push_back(p);
    if (kept != selection_)
        setSelection(kept);
}

int DesktopFiles::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(files_.size());
}

QVariant DesktopFiles::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(files_.size()))
        return {};
    const File& f = files_[size_t(index.row())];
    switch (role) {
    case PathRole: return f.path;
    case NameRole: return f.name;
    case FileNameRole: return f.fileName;
    case IsDirRole: return f.dir;
    case IsImageRole: return f.image;
    case IconRole: return f.icon;
    default: return {};
    }
}

QHash<int, QByteArray> DesktopFiles::roleNames() const {
    return {{PathRole, "path"}, {NameRole, "name"}, {FileNameRole, "fileName"}, {IsDirRole, "isDir"},
            {IsImageRole, "isImage"}, {IconRole, "iconName"}};
}

void DesktopFiles::setSelection(QStringList selection) {
    if (selection == selection_)
        return;
    selection_ = std::move(selection);
    emit selectionChanged();
}

void DesktopFiles::setRenaming(const QString& path) {
    if (path == renaming_)
        return;
    renaming_ = path;
    emit renamingChanged();
}

void DesktopFiles::click(const QString& path, bool ctrl, bool right) {
    setRenaming({});
    if (path.isEmpty()) {
        setSelection({});
    } else if (ctrl) {
        QStringList s = selection_;
        if (!s.removeOne(path))
            s.push_back(path);
        setSelection(s);
    } else if (!right || !selection_.contains(path)) {
        setSelection({path});
    }
}

QStringList DesktopFiles::targets(const QString& path) const {
    return selection_.size() > 1 && selection_.contains(path) ? selection_ : QStringList{path};
}

void DesktopFiles::open(const QString& path) {
    for (const QString& p : targets(path)) {
        if (p.endsWith(".desktop"))
            run("gio", {"launch", p});
        else
            run("xdg-open", {p});
    }
}

void DesktopFiles::openFolder() {
    run("xdg-open", {folder_});
}

void DesktopFiles::terminalHere() {
    QStringList cmd = QProcess::splitCommand(Compositor::instance()->setting("shortcuts.terminal", QString()).toString());
    if (cmd.isEmpty()) {
        run("sh", {"-c", kDefaultTerminal}, folder_);
        return;
    }
    const QString program = cmd.takeFirst();
    run(program, cmd, folder_);
}

void DesktopFiles::newFolder() {
    const QDir dir(folder_);
    const QString name = QString::fromStdString(
        files::free_name("New Folder", [&](const std::string& n) { return dir.exists(QString::fromStdString(n)); }));
    if (!dir.mkdir(name))
        return;
    const QString path = dir.filePath(name);
    reload();  // now, so the icon is there to rename
    setSelection({path});
    setRenaming(path);
}

void DesktopFiles::rename(const QString& path, const QString& name) {
    setRenaming({});
    const QFileInfo from(path);
    if (!files::valid_name(name.toStdString()) || name == from.fileName())
        return;
    const QString to = from.dir().filePath(name);
    if (QFileInfo::exists(to) || !QFile::rename(path, to))
        return;  // never over another file
    if (selection_.contains(path)) {
        QStringList s = selection_;
        s[s.indexOf(path)] = to;
        setSelection(s);
    }
    reload();
}

void DesktopFiles::trash(const QString& path) {
    for (const QString& p : targets(path))
        QFile::moveToTrash(p);
    reload();
}

void DesktopFiles::copyPath(const QString& path) {
    run("wl-copy", {"--", path});
}

bool DesktopFiles::moveIn(const QList<QUrl>& urls) {
    const QDir dir(folder_);
    bool any = false;
    for (const QUrl& url : urls) {
        if (!url.isLocalFile())
            continue;
        const QFileInfo from(url.toLocalFile());
        if (!from.exists() || from.absolutePath() == dir.absolutePath())
            continue;
        const QString to = dir.filePath(from.fileName());
        if (QFileInfo::exists(to))
            continue;
        // A rename within one filesystem; mv copies across them.
        if (!QFile::rename(from.absoluteFilePath(), to))
            run("mv", {"-n", "--", from.absoluteFilePath(), to});
        any = true;
    }
    return any;
}

} // namespace atrium
