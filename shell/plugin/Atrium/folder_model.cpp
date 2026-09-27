#include "folder_model.hpp"
#include "files.hpp"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QStorageInfo>

#include <algorithm>

namespace atrium {

FolderModel::FolderModel(QObject* parent) : QAbstractListModel(parent) {
    settle_.setSingleShot(true);
    settle_.setInterval(150);
    connect(&settle_, &QTimer::timeout, this, &FolderModel::reload);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { settle_.start(); });
    setFolder(QDir::homePath());
}

void FolderModel::setFolder(const QString& path) {
    const QString clean = QDir::cleanPath(path.isEmpty() ? QDir::homePath() : path);
    if (clean == folder_ || !QFileInfo(clean).isDir())
        return;
    if (!folder_.isEmpty())
        watcher_.removePath(folder_);
    folder_ = clean;
    watcher_.addPath(folder_);
    reload();
    emit folderChanged();
}

void FolderModel::setFilter(const QString& filter) {
    if (filter == filter_)
        return;
    filter_ = filter;
    suffixes_ = files::filter_suffixes(filter.toStdString());
    reload();
    emit filterChanged();
}

void FolderModel::setShowHidden(bool on) {
    if (on == showHidden_)
        return;
    showHidden_ = on;
    reload();
    emit showHiddenChanged();
}

void FolderModel::reload() {
    QDir::Filters flags = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (showHidden_)
        flags |= QDir::Hidden;
    const QFileInfoList entries = QDir(folder_).entryInfoList(flags);
    std::vector<File> files;
    const QLocale locale;
    for (const QFileInfo& e : entries) {
        const bool dir = e.isDir();
        if (!dir && !files::has_suffix(e.fileName().toStdString(), suffixes_))
            continue;
        const std::string suffix = e.suffix().toStdString();
        File f;
        f.path = e.absoluteFilePath();
        f.name = e.fileName();
        f.isDir = dir;
        f.isImage = !dir && files::is_image(suffix);
        f.icon = QString::fromStdString(files::icon_for(suffix, dir));
        f.size = dir ? QString() : locale.formattedDataSize(e.size());
        f.modified = locale.toString(e.lastModified(), QLocale::ShortFormat);
        files.push_back(std::move(f));
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::ranges::sort(files, [&](const File& a, const File& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return collator.compare(a.name, b.name) < 0;
    });
    const int before = count();
    beginResetModel();
    files_ = std::move(files);
    endResetModel();
    if (count() != before)
        emit countChanged();
}

QVariantList FolderModel::crumbs() const {
    QVariantList out;
    out.append(QVariantMap{{"name", "/"}, {"path", "/"}});
    QString path;
    for (const QString& part : folder_.split('/', Qt::SkipEmptyParts)) {
        path += "/" + part;
        out.append(QVariantMap{{"name", part}, {"path", path}});
    }
    return out;
}

QVariantList FolderModel::places() const {
    QVariantList out;
    auto add = [&](const QString& name, const QString& icon, const QString& path) {
        if (!path.isEmpty() && QFileInfo(path).isDir())
            out.append(QVariantMap{{"name", name}, {"icon", icon}, {"path", path}});
    };
    using SP = QStandardPaths;
    const QString home = QDir::homePath();
    add("Home", "home", home);
    // Only the ones set up as their own folders (not home itself).
    for (auto [name, icon, where] : {std::tuple{"Desktop", "desktop_windows", SP::DesktopLocation},
                                     std::tuple{"Documents", "description", SP::DocumentsLocation},
                                     std::tuple{"Downloads", "download", SP::DownloadLocation},
                                     std::tuple{"Pictures", "image", SP::PicturesLocation},
                                     std::tuple{"Music", "music_note", SP::MusicLocation},
                                     std::tuple{"Videos", "movie", SP::MoviesLocation}}) {
        const QString path = SP::writableLocation(where);
        if (path != home)
            add(name, icon, path);
    }
    // Drives mounted for the user (removable ones, other disks).
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        const QString root = v.rootPath();
        if (v.isValid() && v.isReady() && (root.startsWith("/run/media/") || root.startsWith("/media/") || root.startsWith("/mnt/")))
            add(v.displayName(), "hard_drive", root);
    }
    return out;
}

int FolderModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant FolderModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count())
        return {};
    const File& f = files_[size_t(index.row())];
    switch (role) {
    case PathRole: return f.path;
    case NameRole: return f.name;
    case IsDirRole: return f.isDir;
    case IsImageRole: return f.isImage;
    case IconRole: return f.icon;
    case SizeRole: return f.size;
    case ModifiedRole: return f.modified;
    default: return {};
    }
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {{PathRole, "path"}, {NameRole, "name"}, {IsDirRole, "isDir"}, {IsImageRole, "isImage"},
            {IconRole, "icon"}, {SizeRole, "size"}, {ModifiedRole, "modified"}};
}

void FolderModel::up() {
    if (folder_ != "/")
        setFolder(QFileInfo(folder_).path());
}

QVariantMap FolderModel::resolve(const QString& typed) const {
    const QString path = QString::fromStdString(
        files::resolve_typed(typed.toStdString(), folder_.toStdString(), QDir::homePath().toStdString()));
    const QFileInfo info(path);
    const bool allowed = info.isFile() && files::has_suffix(info.fileName().toStdString(), suffixes_);
    return {{"path", path}, {"exists", info.exists()}, {"isDir", info.isDir()}, {"allowed", allowed}};
}

int FolderModel::indexOf(const QString& path) const {
    for (size_t i = 0; i < files_.size(); ++i)
        if (files_[i].path == path)
            return int(i);
    return -1;
}

} // namespace atrium
