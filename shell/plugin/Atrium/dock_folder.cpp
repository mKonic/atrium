#include "dock_folder.hpp"

#include "default_apps.hpp"
#include "desktop_entries.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStandardPaths>

#include <algorithm>
#include <thread>

namespace atrium {

namespace {

QString trashDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
}

// Still being written: browsers' and download managers' partial files.
bool partial(const QString& name) {
    for (const char* ext : {".part", ".crdownload", ".download", ".partial", ".tmp", ".!qB", ".aria2"})
        if (name.endsWith(QLatin1String(ext)))
            return true;
    return name.startsWith('.');
}

} // namespace

DockFolder::DockFolder(QObject* parent) : QObject(parent) {
    settle_.setSingleShot(true);
    settle_.setInterval(250);
    connect(&settle_, &QTimer::timeout, this, &DockFolder::rescan);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { settle_.start(); });
}

void DockFolder::setKind(const QString& kind) {
    if (kind == kind_)
        return;
    kind_ = kind;
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    const QString dir = listed();
    QDir().mkpath(dir);
    watcher_.addPath(dir);
    scanned_ = false;
    emit kindChanged();
    rescan();
}

QString DockFolder::name() const {
    return kind_ == "trash" ? tr("Trash") : QFileInfo(path()).fileName();
}

QString DockFolder::path() const {
    if (kind_ == "trash")
        return trashDir();
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

QString DockFolder::listed() const {
    return kind_ == "trash" ? trashDir() + "/files" : path();
}

void DockFolder::rescan() {
    QDir dir(listed());
    const QFileInfoList all = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    QFileInfoList list;
    for (const QFileInfo& f : all)
        if (kind_ == "trash" || !partial(f.fileName()))
            list << f;
    // Newest first: by when it landed there (birth, else last change).
    auto landed = [](const QFileInfo& f) {
        const QDateTime born = f.birthTime();
        return born.isValid() ? std::max(born, f.lastModified()) : f.lastModified();
    };
    std::ranges::stable_sort(list, [&](const QFileInfo& a, const QFileInfo& b) { return landed(a) > landed(b); });

    QMimeDatabase mimes;
    QVariantList items;
    QStringList names;
    for (const QFileInfo& f : list) {
        names << f.fileName();
        if (items.size() >= shown_)
            continue;
        const QMimeType mime = f.isDir() ? mimes.mimeTypeForName("inode/directory") : mimes.mimeTypeForFile(f);
        const bool picture = mime.name().startsWith("image/") && f.size() < 64 * 1024 * 1024;
        items.push_back(QVariantMap{{"name", f.fileName()},
                                    {"path", f.absoluteFilePath()},
                                    {"url", QUrl::fromLocalFile(f.absoluteFilePath())},
                                    {"icon", f.isDir() ? QString("folder") : mime.iconName()},
                                    {"fallback", mime.genericIconName()},
                                    {"image", picture ? QUrl::fromLocalFile(f.absoluteFilePath()).toString() : QString()},
                                    {"dir", f.isDir()}});
    }
    const bool arrival =
        scanned_ && std::ranges::any_of(names, [&](const QString& n) { return !known_.contains(n); });
    known_ = names;
    scanned_ = true;
    if (items != items_ || int(list.size()) != count_) {
        items_ = items;
        count_ = int(list.size());
        emit changed();
    }
    if (arrival && kind_ == "downloads")
        emit arrived();
}

void DockFolder::openFolder() const {
    const QString target = kind_ == "trash" ? QStringLiteral("trash:///") : QUrl::fromLocalFile(path()).toString();
    const QString id = DefaultApps::defaultFor("inode/directory");
    if (auto* e = shell::DesktopEntries::instance()->byId(id)) {
        e->open({target});
        return;
    }
    QDesktopServices::openUrl(QUrl(target));
}

void DockFolder::openItem(const QString& path) const {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void DockFolder::trash(const QList<QUrl>& urls) const {
    for (const QUrl& url : urls)
        if (url.isLocalFile())
            QFile::moveToTrash(url.toLocalFile());
}

void DockFolder::empty() const {
    // Away from the shell's thread: a large Trash takes a while.
    const QString root = trashDir();
    std::thread([root] {
        for (const char* sub : {"/files", "/info", "/expunged"}) {
            QDir dir(root + sub);
            for (const QFileInfo& f : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
                if (f.isDir() && !f.isSymLink())
                    QDir(f.absoluteFilePath()).removeRecursively();
                else
                    QFile::remove(f.absoluteFilePath());
            }
        }
        QFile::remove(root + "/directorysizes");
    }).detach();
}

} // namespace atrium
