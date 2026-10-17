#include "desktop_files.hpp"

#include "compositor.hpp"
#include "default_apps.hpp"
#include "desktop_entries.hpp"
#include "list_sync.hpp"
#include "placeholders.hpp"
#include "terminal.hpp"

#include <QClipboard>
#include <QCollator>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStyleHints>

#include <algorithm>
#include <functional>

namespace atrium {

namespace {

using shell::DesktopEntries;
using shell::DesktopEntry;

QString desktopFolder() {
    if (QString dir = qEnvironmentVariable("ATRIUM_DESKTOP_DIR"); !dir.isEmpty())
        return dir;
    // The XDG desktop folder, which need not be ~/Desktop (Qt reads user-dirs.dirs).
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (dir.isEmpty() || QDir(dir) == QDir::home())
        dir = QDir::homePath() + "/Desktop";
    return dir;
}

// Where icons put by hand are remembered, per desktop folder.
QString positionsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/atrium/desktop-icons.json";
}

void run(const QString& program, const QStringList& args, const QString& cwd = {}) {
    QProcess::startDetached(program, args, cwd);
}

bool taken(const QDir& dir, const std::string& name) {
    return QFileInfo::exists(dir.filePath(QString::fromStdString(name)));
}

QString freeFileName(const QDir& dir, const QString& name) {
    return QString::fromStdString(
        files::free_file_name(name.toStdString(), [&](const std::string& n) { return taken(dir, n); }));
}

// A move, or the mv that copies across filesystems where a rename can't.
void move(const QString& from, const QString& to) {
    if (!QFile::rename(from, to))
        run("mv", {"-n", "--", from, to});
}

// Puts back what QFile::moveToTrash put at `inTrash`, and its .trashinfo.
bool restore(const QString& original, const QString& inTrash) {
    if (QFileInfo::exists(original) || !QFile::rename(inTrash, original))
        return false;
    const QFileInfo f(inTrash);
    QFile::remove(f.dir().filePath("../info/" + f.fileName() + ".trashinfo"));
    return true;
}

bool removeForGood(const QString& path) {
    const QFileInfo f(path);
    if (f.isDir() && !f.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

QMimeData* fileData(const QStringList& paths, bool cut) {
    auto* data = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString& p : paths) {
        urls << QUrl::fromLocalFile(p);
        gnome += "\n" + urls.back().toEncoded();
    }
    data->setUrls(urls);
    // GTK file managers read this one, KDE's the other.
    data->setData("x-special/gnome-copied-files", gnome);
    if (cut)
        data->setData("application/x-kde-cutselection", "1");
    data->setText(paths.join('\n'));
    return data;
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

    renameLater_.setSingleShot(true);
    renameLater_.setInterval(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    connect(&renameLater_, &QTimer::timeout, this, [this] { startRename(renameCandidate_); });
    typedReset_.setSingleShot(true);
    typedReset_.setInterval(800);
    connect(&typedReset_, &QTimer::timeout, this, [this] { typed_.clear(); });

    if (QClipboard* c = QGuiApplication::clipboard())
        connect(c, &QClipboard::dataChanged, this, &DesktopFiles::canPasteChanged);

    auto readSettings = [this] {
        Compositor* c = Compositor::instance();
        const QString sort = c->setting("desktop.sort", "manual").toString();
        const bool hidden = c->setting("desktop.hidden_files", false).toBool();
        if (sort == sort_ && hidden == hidden_)
            return;
        const bool sortChanged = sort != sort_;
        sort_ = sort;
        hidden_ = hidden;
        if (sortChanged)
            emit this->sortByChanged();
        reload();
    };
    connect(Compositor::instance(), &Compositor::settingsChanged, this, readSettings);
    loadPositions();
    readSettings();
    reload();
}

// --- The files and where they go ----------------------------------------------------

void DesktopFiles::reload() {
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (hidden_)
        filters |= QDir::Hidden;
    const QFileInfoList infos = QDir(folder_).entryInfoList(filters);
    const QMimeDatabase mimes;

    std::vector<File> next;
    for (const QFileInfo& i : infos) {
        File f{.path = i.absoluteFilePath(), .name = i.fileName(), .fileName = i.fileName(), .dir = i.isDir()};
        const std::string suffix = i.suffix().toStdString();
        f.image = !f.dir && files::is_image(suffix);
        f.icon = QString::fromStdString(files::icon_for(suffix, f.dir));
        f.link = i.isSymLink();
        f.hidden = i.isHidden();
        f.size = f.dir ? 0 : i.size();
        f.modified = i.lastModified().toMSecsSinceEpoch();
        f.kind = f.dir ? "inode/directory" : mimes.mimeTypeForFile(i, QMimeDatabase::MatchExtension).name();
        // Launchers (Steam's shortcuts) name and draw themselves.
        if (!f.dir && i.suffix() == "desktop") {
            QFile file(f.path);
            if (file.size() < 1 << 20 && file.open(QIODevice::ReadOnly)) {
                const files::Launcher l = files::parse_launcher(file.readAll().toStdString());
                if (!l.name.empty())
                    f.name = QString::fromStdString(l.name);
                f.icon = l.icon.empty() ? "application-x-executable" : QString::fromStdString(l.icon);
                f.launcher = true;
            }
        }
        next.push_back(std::move(f));
    }
    apply(std::move(next));
}

void DesktopFiles::layout(std::vector<File>& files) const {
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const QString by = sort_;
    // Folders first, then by the sort (newest and largest first), then by name.
    std::ranges::stable_sort(files, [&](const File& a, const File& b) {
        if (a.dir != b.dir)
            return a.dir;
        if (by == "kind" && a.kind != b.kind)
            return a.kind < b.kind;
        if (by == "date" && a.modified != b.modified)
            return a.modified > b.modified;
        if (by == "size" && a.size != b.size)
            return a.size > b.size;
        return collator.compare(a.name, b.name) < 0;
    });
    std::vector<std::string> names;
    std::map<std::string, files::Cell> saved;
    for (const File& f : files) {
        names.push_back(f.fileName.toStdString());
        if (auto it = landing_.find(f.fileName); it != landing_.end())
            saved[names.back()] = it->second;
        else if (auto at = positions_.find(f.fileName); by == "manual" && at != positions_.end())
            saved[names.back()] = at->second;
    }
    const std::vector<files::Cell> cells = files::place(names, saved, cols(), rows());
    for (size_t i = 0; i < files.size(); ++i)
        files[i].cell = cells[i];
}

void DesktopFiles::apply(std::vector<File> next) {
    layout(next);
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

    // Files that came in are where they were dropped; by hand, they stay.
    for (auto it = landing_.begin(); it != landing_.end();)
        it = indexOf(folder_ + "/" + it->first) >= 0 ? landing_.erase(it) : std::next(it);
    if (sort_ == "manual")
        savePositions();

    // Whatever is gone is no longer selected.
    QStringList kept;
    for (const QString& p : selection_)
        if (indexOf(p) >= 0)
            kept.push_back(p);
    setSelection(kept);
    if (indexOf(current_) < 0)
        current_.clear();
}

void DesktopFiles::loadPositions() {
    QFile f(positionsFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject mine = QJsonDocument::fromJson(f.readAll()).object().value(folder_).toObject();
    for (auto it = mine.begin(); it != mine.end(); ++it) {
        const QJsonArray at = it.value().toArray();
        positions_[it.key()] = {at.at(0).toInt(), at.at(1).toInt()};
    }
    written_ = positions_;
}

void DesktopFiles::savePositions() {
    // Before the grid is known, everything is in one column: not worth keeping.
    if (area_.isEmpty())
        return;
    positions_.clear();
    for (const File& f : files_)
        positions_[f.fileName] = f.cell;
    if (positions_ == written_)
        return;
    written_ = positions_;
    QFile in(positionsFile());
    QJsonObject all = in.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(in.readAll()).object() : QJsonObject();
    QJsonObject mine;
    for (const auto& [name, c] : positions_)
        mine.insert(name, QJsonArray{c.col, c.row});
    all.insert(folder_, mine);
    QDir().mkpath(QFileInfo(positionsFile()).path());
    QSaveFile out(positionsFile());
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

void DesktopFiles::fixPositions() {
    positions_.clear();
    for (const File& f : files_)
        positions_[f.fileName] = f.cell;
    if (sort_ != "manual")
        setSortBy("manual");
}

int DesktopFiles::rows() const {
    return cell_.height() > 0 ? std::max(1, int((area_.height() - margin_ * 2) / cell_.height())) : 1;
}

int DesktopFiles::cols() const {
    return cell_.width() > 0 ? std::max(1, int((area_.width() - margin_ * 2) / cell_.width())) : 1;
}

void DesktopFiles::setArea(QSizeF area) {
    if (area == area_)
        return;
    area_ = area;
    emit gridChanged();
    apply(files_);
}

void DesktopFiles::setCell(QSizeF cell) {
    if (cell == cell_ || cell.isEmpty())
        return;
    cell_ = cell;
    emit gridChanged();
    apply(files_);
}

void DesktopFiles::setMargin(int margin) {
    if (margin == margin_)
        return;
    margin_ = margin;
    emit gridChanged();
    apply(files_);
}

void DesktopFiles::setSortBy(const QString& sort) {
    if (sort == sort_)
        return;
    // Sorting by hand starts from where they are now.
    if (sort == "manual") {
        positions_.clear();
        for (const File& f : files_)
            positions_[f.fileName] = f.cell;
    }
    sort_ = sort;
    emit sortByChanged();
    Compositor::instance()->setSetting("desktop.sort", sort);
    apply(files_);
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
    case IsLinkRole: return f.link;
    case IsHiddenRole: return f.hidden;
    case SelectedRole: return selection_.contains(f.path);
    case LeftRole: return margin_ + f.cell.col * cell_.width();
    case TopRole: return margin_ + f.cell.row * cell_.height();
    default: return {};
    }
}

QHash<int, QByteArray> DesktopFiles::roleNames() const {
    return {{PathRole, "path"},       {NameRole, "name"},         {FileNameRole, "fileName"},
            {IsDirRole, "isDir"},     {IsImageRole, "isImage"},   {IconRole, "iconName"},
            {IsLinkRole, "isLink"},   {IsHiddenRole, "isHidden"}, {SelectedRole, "selected"},
            {LeftRole, "gridX"},      {TopRole, "gridY"}};
}

int DesktopFiles::indexOf(const QString& path) const {
    for (size_t i = 0; i < files_.size(); ++i)
        if (files_[i].path == path)
            return int(i);
    return -1;
}

files::Cell DesktopFiles::cellAt(qreal x, qreal y) const {
    return {std::clamp(int((x - margin_) / cell_.width()), 0, cols() - 1),
            std::clamp(int((y - margin_) / cell_.height()), 0, rows() - 1)};
}

int DesktopFiles::at(qreal x, qreal y) const {
    if (x < margin_ || y < margin_)
        return -1;
    const files::Cell c{int((x - margin_) / cell_.width()), int((y - margin_) / cell_.height())};
    for (size_t i = 0; i < files_.size(); ++i)
        if (files_[i].cell == c)
            return int(i);
    return -1;
}

std::vector<int> DesktopFiles::gridOrder() const {
    std::vector<files::Cell> cells;
    for (const File& f : files_)
        cells.push_back(f.cell);
    return files::grid_order(cells);
}

// --- Selection --------------------------------------------------------------------------

void DesktopFiles::setSelection(QStringList selection) {
    if (selection == selection_)
        return;
    const QStringList before = std::exchange(selection_, std::move(selection));
    for (size_t i = 0; i < files_.size(); ++i)
        if (before.contains(files_[i].path) != selection_.contains(files_[i].path))
            emit dataChanged(index(int(i)), index(int(i)), {SelectedRole});
    emit selectionChanged();
}

void DesktopFiles::selectOnly(const QString& path) {
    setSelection(path.isEmpty() ? QStringList() : QStringList{path});
    setCurrent(path);
    anchor_ = path;
}

void DesktopFiles::setCurrent(const QString& path) {
    current_ = path;
}

void DesktopFiles::setRenaming(const QString& path) {
    renameLater_.stop();
    if (path == renaming_)
        return;
    renaming_ = path;
    emit renamingChanged();
}

void DesktopFiles::startRename(const QString& path) {
    if (indexOf(path) < 0)
        return;
    selectOnly(path);
    setRenaming(path);
}

void DesktopFiles::selectRange(const QString& to) {
    const std::vector<int> order = gridOrder();
    const auto pos = [&](const QString& p) {
        const int i = indexOf(p);
        return int(std::ranges::find(order, i) - order.begin());
    };
    const int from = indexOf(anchor_) >= 0 ? pos(anchor_) : pos(to);
    const int end = pos(to);
    QStringList run;
    for (int k = std::min(from, end); k <= std::max(from, end) && k < int(order.size()); ++k)
        run << files_[size_t(order[size_t(k)])].path;
    setSelection(run);
    setCurrent(to);
}

void DesktopFiles::press(const QString& path, int modifiers, bool right) {
    setRenaming({});
    const auto mods = Qt::KeyboardModifiers(modifiers);
    if (path.isEmpty()) {
        if (!(mods & (Qt::ControlModifier | Qt::ShiftModifier)))
            selectOnly({});
        return;
    }
    soleWhenPressed_ = selection_ == QStringList{path};
    if (mods & Qt::ShiftModifier && !right) {
        selectRange(path);
    } else if (mods & Qt::ControlModifier && !right) {
        QStringList s = selection_;
        if (!s.removeOne(path))
            s.push_back(path);
        setSelection(s);
        setCurrent(path);
        anchor_ = path;
    } else if (!selection_.contains(path)) {
        // A selected one stays with the rest: they may be about to be dragged.
        selectOnly(path);
    } else {
        setCurrent(path);
        anchor_ = path;
    }
}

void DesktopFiles::release(const QString& path, int modifiers, bool onName) {
    if (path.isEmpty() || Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::ShiftModifier))
        return;
    selectOnly(path);
    if (onName && soleWhenPressed_) {
        renameCandidate_ = path;
        renameLater_.start();
    }
}

void DesktopFiles::activate(const QString& path) {
    renameLater_.stop();
    open(path);
}

void DesktopFiles::beginBand(qreal x, qreal y, int modifiers) {
    setRenaming({});
    bandStart_ = {x, y};
    pinned_ = Qt::KeyboardModifiers(modifiers) & Qt::ControlModifier ? selection_ : QStringList();
    if (pinned_.isEmpty())
        selectOnly({});
}

void DesktopFiles::moveBand(qreal x, qreal y) {
    band_ = QRectF(bandStart_, QPointF(x, y)).normalized();
    emit bandChanged();
    // An icon is its cell less the room around the picture and the name.
    const qreal inset = std::min(cell_.width(), cell_.height()) * 0.12;
    QStringList s = pinned_;
    for (const int i : gridOrder()) {
        const File& f = files_[size_t(i)];
        const QRectF r(margin_ + f.cell.col * cell_.width(), margin_ + f.cell.row * cell_.height(), cell_.width(),
                       cell_.height());
        if (!band_.intersects(r.adjusted(inset, 0, -inset, -inset)))
            continue;
        if (!s.removeOne(f.path))
            s.push_back(f.path);
    }
    setSelection(s);
    if (!s.isEmpty()) {
        setCurrent(s.front());
        anchor_ = s.front();
    }
}

void DesktopFiles::endBand() {
    band_ = {};
    pinned_.clear();
    emit bandChanged();
}

QStringList DesktopFiles::targets(const QString& path) const {
    return selection_.size() > 1 && selection_.contains(path) ? selection_ : QStringList{path};
}

QPointF DesktopFiles::spot(const QString& path) const {
    const int i = indexOf(path);
    if (i < 0)
        return {qreal(margin_), qreal(margin_)};
    const files::Cell c = files_[size_t(i)].cell;
    return {margin_ + (c.col + 0.5) * cell_.width(), margin_ + (c.row + 0.6) * cell_.height()};
}

// --- Keys -------------------------------------------------------------------------------

void DesktopFiles::step(int dx, int dy, int modifiers) {
    std::vector<files::Cell> cells;
    for (const File& f : files_)
        cells.push_back(f.cell);
    const int i = files::nearest(cells, indexOf(current_), dx, dy);
    if (i < 0)
        return;
    const QString to = files_[size_t(i)].path;
    if (Qt::KeyboardModifiers(modifiers) & Qt::ShiftModifier) {
        if (anchor_.isEmpty())
            anchor_ = current_.isEmpty() ? to : current_;
        selectRange(to);
    } else {
        selectOnly(to);
    }
}

bool DesktopFiles::key(int key, int modifiers, const QString& text) {
    const auto mods = Qt::KeyboardModifiers(modifiers) & ~Qt::KeypadModifier;
    const bool ctrl = mods & Qt::ControlModifier, shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    const QString focus = selection_.contains(current_) ? current_ : selection_.value(0);
    if (ctrl && !alt) {
        switch (key) {
        case Qt::Key_A: {
            QStringList all;
            for (const int i : gridOrder())
                all << files_[size_t(i)].path;
            setSelection(all);
            return true;
        }
        case Qt::Key_C: if (!focus.isEmpty()) copy(focus); return true;
        case Qt::Key_X: if (!focus.isEmpty()) cut(focus); return true;
        case Qt::Key_V: paste(); return true;
        case Qt::Key_Z: undo(); return true;
        case Qt::Key_R: reload(); return true;
        case Qt::Key_N: if (shift) newFolder(); return shift;
        default: return false;
        }
    }
    if (alt)
        return false;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!focus.isEmpty())
            open(focus);
        return true;
    case Qt::Key_Delete:
        if (selection_.isEmpty())
            return true;
        if (shift)
            emit deleteRequested(selection_);
        else
            trash(focus);
        return true;
    case Qt::Key_F2:
        if (selection_.size() == 1)
            startRename(selection_.front());
        return true;
    case Qt::Key_F5: reload(); return true;
    case Qt::Key_F10:
        if (!shift)
            return false;
        [[fallthrough]];
    case Qt::Key_Menu: emit menuRequested(focus); return true;
    case Qt::Key_Escape:
        if (selection_.isEmpty())
            return false;
        selectOnly({});
        return true;
    case Qt::Key_Left: step(-1, 0, mods); return true;
    case Qt::Key_Right: step(1, 0, mods); return true;
    case Qt::Key_Up: step(0, -1, mods); return true;
    case Qt::Key_Down: step(0, 1, mods); return true;
    case Qt::Key_Home:
    case Qt::Key_End: {
        const std::vector<int> order = gridOrder();
        if (order.empty())
            return true;
        const QString to = files_[size_t(key == Qt::Key_Home ? order.front() : order.back())].path;
        if (shift)
            selectRange(to);
        else
            selectOnly(to);
        return true;
    }
    default: break;
    }
    // Typing a name's start selects it.
    if (text.size() == 1 && text.front().isPrint() && !text.front().isSpace()) {
        typed_ += text;
        typedReset_.start();
        std::vector<std::string> names;
        for (const File& f : files_)
            names.push_back(f.name.toStdString());
        const int i = files::type_ahead(names, gridOrder(), indexOf(current_), typed_.toStdString());
        if (i >= 0)
            selectOnly(files_[size_t(i)].path);
        return true;
    }
    return false;
}

// --- What the menu does -------------------------------------------------------------

void DesktopFiles::open(const QString& path) {
    for (const QString& p : targets(path)) {
        if (p.endsWith(".desktop"))
            run("gio", {"launch", p});
        else
            run("xdg-open", {p});
    }
}

QVariantList DesktopFiles::apps(const QString& path) const {
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(path);
    QStringList types = mime.allAncestors();
    types.prepend(mime.name());
    const QString preferred = DefaultApps::defaultFor(mime.name());
    QList<DesktopEntry*> found;
    for (QObject* o : DesktopEntries::instance()->applications()->values()) {
        auto* e = qobject_cast<DesktopEntry*>(o);
        if (e && !e->noDisplay() && std::ranges::any_of(types, [&](const QString& t) { return e->mimeTypes().contains(t); }))
            found.append(e);
    }
    QCollator order;
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::ranges::sort(found, [&](DesktopEntry* a, DesktopEntry* b) {
        if ((a->id() == preferred) != (b->id() == preferred))
            return a->id() == preferred;
        return order.compare(a->name(), b->name()) < 0;
    });
    QVariantList out;
    for (DesktopEntry* e : found)
        out.append(QVariantMap{{"value", e->id()}, {"label", e->name()}, {"icon", e->icon()}});
    return out;
}

void DesktopFiles::openWith(const QString& path, const QString& appId) {
    DesktopEntry* e = DesktopEntries::instance()->byId(appId);
    if (!e)
        return;
    QStringList urls;
    for (const QString& p : targets(path))
        urls << QUrl::fromLocalFile(p).toString();
    e->open(urls);
}

void DesktopFiles::openFolder() {
    run("xdg-open", {folder_});
}

void DesktopFiles::terminalHere() {
    const QString configured = Compositor::instance()->setting("shortcuts.terminal", QString()).toString();
    const std::string dir = placeholders::shell_quote(folder_.toStdString());
    run("sh", {"-c", QString::fromStdString(terminal_in(configured.toStdString(), dir))}, folder_);
}

void DesktopFiles::newFolder() {
    const QDir dir(folder_);
    const QString name = QString::fromStdString(
        files::free_name("New Folder", [&](const std::string& n) { return taken(dir, n); }));
    if (!dir.mkdir(name))
        return;
    const QString path = dir.filePath(name);
    pushUndo({.text = tr("New Folder"), .made = {path}});
    reload();  // now, so the icon is there to rename
    startRename(path);
}

QVariantList DesktopFiles::templates() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation);
    QVariantList out;
    if (dir.isEmpty() || QDir(dir) == QDir::home())
        return out;
    QCollator order;
    order.setCaseSensitivity(Qt::CaseInsensitive);
    QFileInfoList infos = QDir(dir).entryInfoList(QDir::Files);
    std::ranges::sort(infos, [&](const QFileInfo& a, const QFileInfo& b) {
        return order.compare(a.completeBaseName(), b.completeBaseName()) < 0;
    });
    for (const QFileInfo& i : infos)
        out.append(QVariantMap{{"label", i.completeBaseName()},
                               {"path", i.absoluteFilePath()},
                               {"icon", QString::fromStdString(files::icon_for(i.suffix().toStdString(), false))}});
    return out;
}

void DesktopFiles::newFile(const QString& templatePath) {
    const QDir dir(folder_);
    const QString name = freeFileName(dir, templatePath.isEmpty() ? tr("New Text File") + ".txt"
                                                                  : QFileInfo(templatePath).fileName());
    const QString path = dir.filePath(name);
    if (templatePath.isEmpty()) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return;
    } else if (!QFile::copy(templatePath, path)) {
        return;
    }
    pushUndo({.text = tr("New Document"), .made = {path}});
    reload();
    startRename(path);
}

void DesktopFiles::rename(const QString& path, const QString& name) {
    setRenaming({});
    const QFileInfo from(path);
    if (!files::valid_name(name.toStdString()) || name == from.fileName())
        return;
    const QString to = from.dir().filePath(name);
    if (QFileInfo::exists(to) || !QFile::rename(path, to))
        return;  // never over another file
    // It keeps its place.
    if (auto it = positions_.find(from.fileName()); it != positions_.end()) {
        positions_[name] = it->second;
        positions_.erase(from.fileName());
    }
    pushUndo({.text = tr("Rename"), .moved = {{path, to}}});
    if (selection_.contains(path)) {
        QStringList s = selection_;
        s[s.indexOf(path)] = to;
        setSelection(s);
    }
    if (current_ == path)
        current_ = to;
    reload();
}

void DesktopFiles::trash(const QString& path) {
    Step s{.text = tr("Move to Trash")};
    for (const QString& p : targets(path)) {
        QString where;
        if (QFile::moveToTrash(p, &where))
            s.trashed.emplace_back(p, where);
    }
    if (!s.trashed.empty())
        pushUndo(std::move(s));
    reload();
}

void DesktopFiles::deleteNow(const QStringList& paths) {
    for (const QString& p : paths)
        removeForGood(p);
    reload();
}

void DesktopFiles::copy(const QString& path) {
    QGuiApplication::clipboard()->setMimeData(fileData(targets(path), false));
}

void DesktopFiles::cut(const QString& path) {
    QGuiApplication::clipboard()->setMimeData(fileData(targets(path), true));
}

bool DesktopFiles::canPaste() const {
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    return data && std::ranges::any_of(data->urls(), [](const QUrl& u) { return u.isLocalFile(); });
}

void DesktopFiles::paste(const QString& into) {
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    if (!data)
        return;
    const bool cut = data->data("application/x-kde-cutselection") == "1" ||
                     data->data("x-special/gnome-copied-files").startsWith("cut");
    const QList<QUrl> urls = data->urls();
    const QStringList made = transfer(urls, into.isEmpty() ? folder_ : into, !cut, cut ? tr("Move") : tr("Paste"));
    // Moved is moved: pasting again would find nothing there.
    if (cut && !made.isEmpty())
        QGuiApplication::clipboard()->clear();
    if (into.isEmpty())
        setSelection(made);
}

QStringList DesktopFiles::transfer(const QList<QUrl>& urls, const QString& dir, bool copy, const QString& text) {
    const QDir to(dir);
    Step s{.text = text};
    QStringList out;
    for (const QUrl& url : urls) {
        if (!url.isLocalFile())
            continue;
        const QFileInfo from(url.toLocalFile());
        if (!from.exists() || (!copy && QDir(from.absolutePath()) == to) || from.absoluteFilePath() == to.absolutePath())
            continue;
        // Never into itself.
        if (from.isDir() && (to.absolutePath() + "/").startsWith(from.absoluteFilePath() + "/"))
            continue;
        const QString target = to.filePath(freeFileName(to, from.fileName()));
        if (copy) {
            run("cp", {"-a", "--", from.absoluteFilePath(), target});
            s.made << target;
        } else {
            move(from.absoluteFilePath(), target);
            s.moved.emplace_back(from.absoluteFilePath(), target);
        }
        out << target;
    }
    if (!out.isEmpty())
        pushUndo(std::move(s));
    reload();
    return out;
}

void DesktopFiles::pushUndo(Step s) {
    undo_.push_back(std::move(s));
    // As far back as Plasma's, and no further.
    if (undo_.size() > 50)
        undo_.erase(undo_.begin());
    emit undoChanged();
}

QString DesktopFiles::undoText() const {
    return undo_.empty() ? QString() : undo_.back().text;
}

void DesktopFiles::undo() {
    if (undo_.empty())
        return;
    const Step s = std::move(undo_.back());
    undo_.pop_back();
    for (const auto& [from, to] : s.moved) {
        if (!QFileInfo::exists(from))
            move(to, from);
        if (QFileInfo(from).dir() == QDir(folder_) && QFileInfo(to).dir() == QDir(folder_))
            if (auto it = positions_.find(QFileInfo(to).fileName()); it != positions_.end())
                positions_[QFileInfo(from).fileName()] = it->second;
    }
    for (const auto& [original, inTrash] : s.trashed)
        restore(original, inTrash);
    for (const QString& p : s.made)
        QFile::moveToTrash(p);
    emit undoChanged();
    reload();
}

void DesktopFiles::copyPath(const QString& path) {
    Compositor::instance()->clipboard("set", {{"text", path}});
}

void DesktopFiles::showOriginal(const QString& path) {
    const QString target = QFileInfo(path).symLinkTarget();
    if (target.isEmpty())
        return;
    QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                    "org.freedesktop.FileManager1", "ShowItems");
    m << QStringList{QUrl::fromLocalFile(target).toString()} << QString();
    QDBusConnection::sessionBus().asyncCall(m);
}

// --- Dragging -----------------------------------------------------------------------

QString DesktopFiles::dragUris(const QString& path) {
    dragAnchor_ = path;
    QStringList lines;
    for (const QString& p : targets(path))
        lines << QString::fromUtf8(QUrl::fromLocalFile(p).toEncoded());
    return lines.join("\r\n") + "\r\n";
}

void DesktopFiles::dragOver(qreal x, qreal y, const QList<QUrl>& urls) {
    QString target;
    if (const int i = at(x, y); i >= 0) {
        const File& f = files_[size_t(i)];
        const bool dragged = std::ranges::any_of(urls, [&](const QUrl& u) { return u.toLocalFile() == f.path; });
        if (!dragged && (f.dir || f.launcher))
            target = f.path;
    }
    if (target != dropTarget_) {
        dropTarget_ = target;
        emit dropTargetChanged();
    }
}

bool DesktopFiles::drop(const QList<QUrl>& urls, qreal x, qreal y, bool copy) {
    dragOver(-1, -1, {});
    QStringList paths;
    for (const QUrl& u : urls)
        if (u.isLocalFile())
            paths << QFileInfo(u.toLocalFile()).absoluteFilePath();
    if (paths.isEmpty())
        return false;

    // Onto a folder: in it. Onto an app: opened with it.
    if (const int i = at(x, y); i >= 0 && !paths.contains(files_[size_t(i)].path)) {
        const File& f = files_[size_t(i)];
        if (f.dir) {
            transfer(urls, f.path, copy, copy ? tr("Copy") : tr("Move"));
            return true;
        }
        if (f.launcher) {
            run("gio", QStringList{"launch", f.path} + paths);
            return true;
        }
    }

    const files::Cell drop = cellAt(x, y);
    const bool ours = std::ranges::all_of(paths, [&](const QString& p) { return QFileInfo(p).dir() == QDir(folder_); });
    if (!ours || copy) {
        const QStringList made = transfer(urls, folder_, copy, copy ? tr("Copy") : tr("Move"));
        if (!made.isEmpty()) {
            landing_[QFileInfo(made.front()).fileName()] = drop;
            reload();
            setSelection(made);
        }
        return !made.isEmpty();
    }

    // Our own icons, moved together: each as far as the one grabbed went.
    const int anchor = indexOf(paths.contains(dragAnchor_) ? dragAnchor_ : paths.front());
    if (anchor < 0)
        return false;
    fixPositions();
    const files::Cell from = files_[size_t(anchor)].cell;
    std::map<files::Cell, QString> occupied;
    for (const File& f : files_)
        if (!paths.contains(f.path))
            occupied[f.cell] = f.fileName;
    for (const QString& p : paths) {
        const int i = indexOf(p);
        if (i < 0)
            continue;
        const File& f = files_[size_t(i)];
        files::Cell want{std::clamp(f.cell.col + drop.col - from.col, 0, cols() - 1),
                         std::clamp(f.cell.row + drop.row - from.row, 0, rows() - 1)};
        // A taken cell gives the nearest free one.
        files::Cell best = want;
        for (int r = 0; occupied.contains(best) && r < cols() + rows(); ++r)
            for (int dc = -r; dc <= r && occupied.contains(best); ++dc)
                for (int dr = -r; dr <= r; ++dr) {
                    const files::Cell c{want.col + dc, want.row + dr};
                    if (std::abs(dc) + std::abs(dr) == r && c.col >= 0 && c.row >= 0 && c.col < cols() &&
                        c.row < rows() && !occupied.contains(c)) {
                        best = c;
                        break;
                    }
                }
        occupied[best] = f.fileName;
        positions_[f.fileName] = best;
    }
    apply(files_);
    return true;
}

} // namespace atrium
