#include "folder_model.hpp"
#include "files.hpp"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QLocale>
#include <QMimeDatabase>
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

void FolderModel::setPatterns(const QVariantList& patterns) {
    if (patterns == patterns_)
        return;
    patterns_ = patterns;
    globs_.clear();
    mimes_.clear();
    for (const QVariant& v : patterns) {
        const QVariantMap p = v.toMap();
        if (p.contains("glob"))
            globs_.push_back(p.value("glob").toString().toStdString());
        else if (p.contains("mime"))
            mimes_.append(p.value("mime").toString());
    }
    reload();
    emit filterChanged();
}

void FolderModel::setFoldersOnly(bool on) {
    if (on == foldersOnly_)
        return;
    foldersOnly_ = on;
    reload();
    emit filterChanged();
}

bool FolderModel::lets(const QFileInfo& file) const {
    const std::string name = file.fileName().toStdString();
    if (patterns_.isEmpty())
        return files::has_suffix(name, suffixes_);
    for (const std::string& glob : globs_)
        if (files::glob_match(name, glob))
            return true;
    if (mimes_.isEmpty())
        return false;
    static const QMimeDatabase db;
    const QMimeType type = db.mimeTypeForFile(file);
    QStringList kinds = type.allAncestors();
    kinds.prepend(type.name());
    for (const QString& m : mimes_) {
        if (m == "*" || m == "*/*")
            return true;
        // "image/*": any image.
        if (m.endsWith("/*")) {
            const QString group = m.chopped(1);
            for (const QString& k : std::as_const(kinds))
                if (k.startsWith(group))
                    return true;
        } else if (type.inherits(m)) {
            return true;
        }
    }
    return false;
}

void FolderModel::setShowHidden(bool on) {
    if (on == showHidden_)
        return;
    showHidden_ = on;
    reload();
    emit showHiddenChanged();
}

void FolderModel::reload() {
    // What was picked stays picked while it's still here, in the same folder.
    QSet<QString> picked;
    for (const File& f : files_)
        if (f.selected && QFileInfo(f.path).path() == folder_)
            picked.insert(f.path);
    QDir::Filters flags = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (showHidden_)
        flags |= QDir::Hidden;
    const QFileInfoList entries = QDir(folder_).entryInfoList(flags);
    std::vector<File> files;
    const QLocale locale;
    for (const QFileInfo& e : entries) {
        const bool dir = e.isDir();
        if (!dir && (foldersOnly_ || !lets(e)))
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
        f.selected = picked.contains(f.path);
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
    const QStringList had = selection();
    beginResetModel();
    files_ = std::move(files);
    endResetModel();
    if (count() != before)
        emit countChanged();
    if (selection() != had) {
        anchor_ = -1;
        emit selectionChanged();
    }
}

QStringList FolderModel::selection() const {
    QStringList out;
    for (const File& f : files_)
        if (f.selected)
            out.append(f.path);
    return out;
}

void FolderModel::setMultiple(bool on) {
    if (on == multiple_)
        return;
    multiple_ = on;
    if (!on && selection().size() > 1)
        clearSelection();
    emit selectionChanged();
}

void FolderModel::setSelected(size_t i, bool on) {
    if (files_[i].selected == on)
        return;
    files_[i].selected = on;
    const QModelIndex at = index(int(i));
    emit dataChanged(at, at, {SelectedRole});
}

void FolderModel::select(int index, Pick how) {
    if (index < 0 || index >= count())
        return;
    const QStringList had = selection();
    if (!multiple_)
        how = Only;
    if (how == Toggle) {
        setSelected(size_t(index), !files_[size_t(index)].selected);
        anchor_ = index;
    } else if (how == Extend && anchor_ >= 0 && anchor_ < count()) {
        const auto [from, to] = std::minmax(anchor_, index);
        for (int i = 0; i < count(); ++i)
            setSelected(size_t(i), i >= from && i <= to);
    } else {
        for (int i = 0; i < count(); ++i)
            setSelected(size_t(i), i == index);
        anchor_ = index;
    }
    if (selection() != had)
        emit selectionChanged();
}

void FolderModel::clearSelection() {
    const QStringList had = selection();
    for (size_t i = 0; i < files_.size(); ++i)
        setSelected(i, false);
    anchor_ = -1;
    if (!had.isEmpty())
        emit selectionChanged();
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
    case SelectedRole: return f.selected;
    default: return {};
    }
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {{PathRole, "path"}, {NameRole, "name"}, {IsDirRole, "isDir"}, {IsImageRole, "isImage"},
            {IconRole, "icon"}, {SizeRole, "size"}, {ModifiedRole, "modified"}, {SelectedRole, "selected"}};
}

void FolderModel::up() {
    if (folder_ != "/")
        setFolder(QFileInfo(folder_).path());
}

QVariantMap FolderModel::resolve(const QString& typed) const {
    const QString path = QString::fromStdString(
        files::resolve_typed(typed.toStdString(), folder_.toStdString(), QDir::homePath().toStdString()));
    const QFileInfo info(path);
    const bool allowed = foldersOnly_ ? info.isDir() : info.isFile() && lets(info);
    return {{"path", path}, {"exists", info.exists()}, {"isDir", info.isDir()}, {"allowed", allowed}};
}

int FolderModel::indexOf(const QString& path) const {
    for (size_t i = 0; i < files_.size(); ++i)
        if (files_[i].path == path)
            return int(i);
    return -1;
}

} // namespace atrium
