#include "chooser_model.hpp"

#include "file_search_core.hpp"
#include "files.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <sys/vfs.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace atrium {

namespace {

QString thumbnailFor(const QString& path) {
    // The freedesktop thumbnail cache, as file managers fill it.
    static const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails/";
    const QByteArray md5 =
        QCryptographicHash::hash(QUrl::fromLocalFile(path).toEncoded(), QCryptographicHash::Md5).toHex();
    for (const char* size : {"large/", "normal/"}) {
        const QString t = cache + size + md5 + ".png";
        if (QFileInfo::exists(t))
            return t;
    }
    return {};
}

QString homePath() {
    return QDir::homePath();
}

QString parentOf(const QString& path) {
    return path == "/" ? QString() : QFileInfo(path).path();
}

// g_utf8_collate_key_for_filename's idea: case folded, numbers compared by
// value ("file2" before "file10"), as a byte-comparable key.
std::string collateKey(const QString& name) {
    const QString folded = name.normalized(QString::NormalizationForm_KD).toCaseFolded();
    QString out;
    for (qsizetype i = 0; i < folded.size();) {
        if (folded[i].isDigit()) {
            qsizetype j = i;
            while (j < folded.size() && folded[j].isDigit())
                ++j;
            const QString digits = folded.mid(i, j - i);
            out += QString(20 - std::min<qsizetype>(20, digits.size()), '0') + digits;
            i = j;
        } else {
            out += folded[i++];
        }
    }
    return out.toStdString();
}

// A folder as a sort key: its parts' collation keys, joined by NUL (which no
// name holds), so "a/b" sorts before "a b" and a folder before its subfolders:
// the order the search walks in.
std::string dirKeyFor(const QString& dir) {
    std::string key;
    for (const QString& part : dir.split('/', Qt::SkipEmptyParts)) {
        key += '\0';
        key += collateKey(part);
    }
    return key;
}

// Kernel and device filesystems hold no files to choose (and /proc never ends).
bool pseudoFilesystem(const std::string& dir) {
    struct statfs fs {};
    if (statfs(dir.c_str(), &fs) != 0)
        return true;
    switch (static_cast<unsigned long>(fs.f_type)) {
    case 0x9fa0:      // proc
    case 0x62656572:  // sysfs
    case 0x1cd1:      // devpts
    case 0x27e0eb:    // cgroup
    case 0x63677270:  // cgroup2
    case 0x64626720:  // debugfs
    case 0x74726163:  // tracefs
    case 0x73636673:  // securityfs
    case 0x6165676c:  // pstore
    case 0xcafe4a11:  // bpf
    case 0x62656570:  // configfs
    case 0x65735543:  // fusectl
    case 0xde5e81e4:  // efivarfs
    case 0x42494e4d:  // binfmt_misc
    case 0x958458f6:  // hugetlbfs
    case 0x19800202:  // mqueue
    case 0x0187:      // autofs (entering one mounts it)
        return true;
    default:
        return dir == "/dev";
    }
}

} // namespace

ChooserModel::ChooserModel(QObject* parent) : QAbstractListModel(parent) {
    settle_.setSingleShot(true);
    settle_.setInterval(150);
    connect(&settle_, &QTimer::timeout, this, [this] {
        if (mode_ == "browse")
            loadFolder();
    });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { settle_.start(); });
    queryDelay_.setSingleShot(true);
    queryDelay_.setInterval(150);
    connect(&queryDelay_, &QTimer::timeout, this, [this] {
        if (mode_ == "search")
            runSearch();
    });
}

ChooserModel::~ChooserModel() {
    cancelSearch();
}

chooser::Action ChooserModel::act() const {
    if (action_ == "save")
        return chooser::Action::Save;
    if (action_ == "folder")
        return chooser::Action::SelectFolder;
    return chooser::Action::Open;
}

void ChooserModel::setAction(const QString& action) {
    if (action == action_)
        return;
    action_ = action;
    if (act() == chooser::Action::Save)
        multiple_ = false;
    emit actionChanged();
    reload();
}

void ChooserModel::setFolder(const QString& path) {
    QString clean = QDir::cleanPath(path.isEmpty() ? homePath() : path);
    // A folder that went away: the nearest one above it that's there.
    while (!QFileInfo(clean).isDir() && clean != "/")
        clean = parentOf(clean);
    const bool modeChange = mode_ != "browse";
    if (clean == folder_ && !modeChange)
        return;
    cancelSearch();
    if (!folder_.isEmpty())
        watcher_.removePath(folder_);
    folder_ = clean;
    watcher_.addPath(folder_);
    setTrail(folder_, true);
    setMode("browse");
    anchor_ = -1;
    loadFolder();
    emit folderChanged();
}

void ChooserModel::setQuery(const QString& query) {
    if (query == query_)
        return;
    query_ = query;
    emit queryChanged();
    if (mode_ != "search")
        return;
    // Cleared: at once. Typed: once the typing pauses.
    if (query_.trimmed().isEmpty()) {
        queryDelay_.stop();
        runSearch();
    } else {
        queryDelay_.start();
    }
}

void ChooserModel::setPatterns(const QVariantList& patterns) {
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
    emit patternsChanged();
    reload();
}

void ChooserModel::setShowHidden(bool on) {
    if (on == showHidden_)
        return;
    showHidden_ = on;
    emit viewChanged();
    reload();
}

void ChooserModel::setFoldersFirst(bool on) {
    if (on == foldersFirst_)
        return;
    foldersFirst_ = on;
    emit viewChanged();
    std::vector<Row> rows = rows_;
    setRows(std::move(rows));
}

QString ChooserModel::sortColumn() const {
    switch (sort_) {
    case chooser::SortColumn::Size: return "size";
    case chooser::SortColumn::Type: return "type";
    case chooser::SortColumn::Modified: return "modified";
    default: return "name";
    }
}

void ChooserModel::setSortColumn(const QString& column) {
    const auto c = column == "size"       ? chooser::SortColumn::Size
                   : column == "type"     ? chooser::SortColumn::Type
                   : column == "modified" ? chooser::SortColumn::Modified
                                          : chooser::SortColumn::Name;
    if (c == sort_)
        return;
    sort_ = c;
    emit viewChanged();
    std::vector<Row> rows = rows_;
    setRows(std::move(rows));
}

void ChooserModel::setAscending(bool on) {
    if (on == ascending_)
        return;
    ascending_ = on;
    emit viewChanged();
    std::vector<Row> rows = rows_;
    setRows(std::move(rows));
}

void ChooserModel::setShowTime(bool on) {
    if (on == showTime_)
        return;
    showTime_ = on;
    emit viewChanged();
    if (!rows_.empty())
        emit dataChanged(index(0), index(count() - 1), {DateTextRole, TimeTextRole});
}

void ChooserModel::setClock24(bool on) {
    if (on == clock24_)
        return;
    clock24_ = on;
    emit viewChanged();
    if (!rows_.empty())
        emit dataChanged(index(0), index(count() - 1), {DateTextRole, TimeTextRole});
}

void ChooserModel::setMultiple(bool on) {
    on = on && act() != chooser::Action::Save;
    if (on == multiple_)
        return;
    multiple_ = on;
    if (!on && selection().size() > 1)
        clearSelection();
    emit selectionChanged();
}

ChooserModel::Lens ChooserModel::lens() const {
    return {act(), mode_ != "browse", !patterns_.isEmpty(), globs_, mimes_};
}

bool ChooserModel::passes(const Lens& lens, const QFileInfo& info) {
    // Folders are never filtered: they are there to go into.
    if (!lens.filtered || info.isDir())
        return true;
    const std::string name = info.fileName().toStdString();
    for (const std::string& glob : lens.globs)
        if (files::glob_match(name, glob))
            return true;
    if (lens.mimes.isEmpty())
        return false;
    const QMimeDatabase db;
    const QMimeType type = db.mimeTypeForFile(info);
    for (const QString& m : lens.mimes) {
        if (m == "*" || m == "*/*")
            return true;
        if (m.endsWith("/*")) {
            const QString group = m.chopped(1);
            if (type.name().startsWith(group) ||
                std::ranges::any_of(type.allAncestors(), [&](const QString& a) { return a.startsWith(group); }))
                return true;
        } else if (type.inherits(m)) {
            return true;
        }
    }
    return false;
}

ChooserModel::Row ChooserModel::buildRow(const Lens& lens, const QFileInfo& info, qint64 time) {
    const QMimeDatabase db;
    Row r;
    r.path = info.absoluteFilePath();
    r.name = info.fileName();
    r.isDir = info.isDir();
    r.hidden = r.name.startsWith('.') || r.name.endsWith('~');
    r.sensitive = chooser::sensitive(lens.act, r.isDir);
    r.time = time ? time : info.lastModified().toSecsSinceEpoch();
    r.nameKey = collateKey(r.name);
    if (r.isDir) {
        r.icon = "folder";
    } else {
        const QMimeType type = db.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        r.icon = type.iconName();
        r.size = info.size();
        r.sizeText = QString::fromStdString(chooser::size_text(quint64(r.size)));
        const std::string cat = chooser::category(type.genericIconName().toStdString());
        r.type = cat.empty() ? type.comment() : QString::fromStdString(cat);
        r.thumb = thumbnailFor(r.path);
        if (r.thumb.isEmpty() && type.name().startsWith("image/"))
            r.thumb = r.path;  // the picture itself, scaled down
    }
    // Recent and search say where each one is: home-relative.
    const QString dir = info.absolutePath();
    if (lens.located) {
        r.dirKey = dirKeyFor(dir);
        if (dir == homePath())
            r.location = "Home";
        else if (dir.startsWith(homePath() + "/"))
            r.location = dir.mid(homePath().size() + 1);
        else
            r.location = dir;
    }
    return r;
}

void ChooserModel::sortRows(std::vector<Row>& rows) const {
    auto key = [](const Row& r) {
        return chooser::SortKey{r.isDir, r.nameKey, quint64(r.size), r.type.toStdString(), r.time};
    };
    if (mode_ == "recent") {
        // Newest used first.
        std::ranges::stable_sort(rows, [](const Row& a, const Row& b) {
            return a.time != b.time ? a.time > b.time : a.nameKey < b.nameKey;
        });
        return;
    }
    if (mode_ == "search") {
        // By where, then name: the order the walk finds them in.
        std::ranges::stable_sort(rows, [](const Row& a, const Row& b) {
            if (a.dirKey != b.dirKey)
                return a.dirKey < b.dirKey;
            return a.nameKey != b.nameKey ? a.nameKey < b.nameKey : a.name < b.name;
        });
        return;
    }
    std::ranges::stable_sort(rows, [&](const Row& a, const Row& b) {
        return chooser::sorts_before(key(a), key(b), sort_, ascending_, foldersFirst_);
    });
}

void ChooserModel::setRows(std::vector<Row> rows) {
    // What was picked stays picked while it's still here.
    QSet<QString> picked;
    for (const Row& r : rows_)
        if (r.selected)
            picked.insert(r.path);
    for (Row& r : rows)
        r.selected = r.sensitive && picked.contains(r.path);
    sortRows(rows);
    const int before = count();
    const QStringList had = selection();
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    if (count() != before)
        emit countChanged();
    if (selection() != had) {
        anchor_ = -1;
        emit selectionChanged();
    }
    applyPending();
}

void ChooserModel::loadFolder() {
    QDir dir(folder_);
    QDir::Filters flags = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden_)
        flags |= QDir::Hidden;
    if (!dir.isReadable()) {
        setError(QString("Could not read the contents of %1").arg(QFileInfo(folder_).fileName().isEmpty() ? folder_ : QFileInfo(folder_).fileName()));
        setRows({});
        return;
    }
    setError({});
    std::vector<Row> rows;
    for (const QFileInfo& e : dir.entryInfoList(flags)) {
        if (!showHidden_ && e.fileName().endsWith('~'))  // backups count as hidden
            continue;
        if (!lets(e))
            continue;
        rows.push_back(makeRow(e, 0));
    }
    setRows(std::move(rows));
}

void ChooserModel::loadRecent() {
    std::vector<Row> rows;
    QFile f(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel");
    std::vector<file_search::Recent> recent;
    if (f.open(QIODevice::ReadOnly))
        recent = file_search::parse_recent(f.readAll().toStdString());
    if (act() == chooser::Action::Open) {
        for (const file_search::Recent& r : recent) {
            const QFileInfo info(QString::fromStdString(r.path));
            if (info.exists() && info.isFile() && lets(info))
                rows.push_back(makeRow(info, r.when));
        }
    } else {
        // Saving or choosing a folder: the folders they were in.
        std::vector<std::string> files;
        for (const file_search::Recent& r : recent)
            files.push_back(r.path);
        long long when = 0;
        for (const std::string& d : chooser::recent_folders(files)) {
            for (const file_search::Recent& r : recent)
                if (QFileInfo(QString::fromStdString(r.path)).path() == QString::fromStdString(d)) {
                    when = r.when;
                    break;
                }
            const QFileInfo info(QString::fromStdString(d));
            if (info.isDir())
                rows.push_back(makeRow(info, when));
        }
    }
    setRows(std::move(rows));
}

void ChooserModel::showRecent() {
    cancelSearch();
    setMode("recent");
    setError({});
    loadRecent();
}

void ChooserModel::startSearch() {
    if (mode_ == "search")
        return;
    setMode("search");
    setRows({});
    runSearch();
}

void ChooserModel::stopSearch() {
    if (mode_ != "search")
        return;
    cancelSearch();
    query_.clear();
    emit queryChanged();
    setMode("browse");
    loadFolder();
}

void ChooserModel::cancelSearch() {
    if (searchStop_)
        searchStop_->store(true);
    if (searchThread_.joinable())
        searchThread_.join();
    searchStop_.reset();
    if (searching_) {
        searching_ = false;
        emit searchingChanged();
    }
}

void ChooserModel::runSearch() {
    queryDelay_.stop();
    cancelSearch();
    setRows({});
    const QString text = query_.trimmed();
    if (text.isEmpty())
        return;
    // Every word somewhere in the name, ignoring case; under the folder
    // being browsed, hidden ones left out unless shown. Rows are made on the
    // thread and arrive in their final order (each folder's names sorted,
    // its matches before its subfolders'), so the list only ever grows at
    // the end. Symlinks aren't followed; kernel filesystems aren't entered.
    const std::vector<std::string> terms = file_search::terms(text.toStdString());
    auto stop = std::make_shared<std::atomic<bool>>(false);
    searchStop_ = stop;
    searching_ = true;
    emit searchingChanged();
    const std::string root = QFile::encodeName(folder_).toStdString();
    const bool hidden = showHidden_;
    const Lens lens = this->lens();
    searchThread_ = std::thread([this, stop, root, hidden, terms, lens] {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;
        std::vector<Row> batch;
        auto sent = Clock::now();
        auto send = [&] {
            sent = Clock::now();
            if (batch.empty() || stop->load())
                return;
            auto rows = std::make_shared<std::vector<Row>>(std::move(batch));
            batch.clear();
            QMetaObject::invokeMethod(this, [this, rows, stop] {
                if (!stop->load())
                    appendRows(std::move(*rows));
            }, Qt::QueuedConnection);
        };
        struct Entry {
            std::string name, key;
            QString qname;
            bool dir = false;  // a real folder (not a link to one): walked into
        };
        auto visit = [&](auto& self, const std::string& dir, int depth) -> void {
            if (stop->load() || depth > 64 || pseudoFilesystem(dir))
                return;
            std::error_code ec;
            fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
            if (ec)
                return;
            std::vector<Entry> entries;
            for (const fs::directory_entry& e : it) {
                if (stop->load())
                    return;
                std::string name = e.path().filename().string();
                if (!hidden && (name.starts_with('.') || name.ends_with('~')))
                    continue;
                Entry entry;
                entry.qname = QFile::decodeName(QByteArray::fromStdString(name));
                entry.key = collateKey(entry.qname);
                // The type readdir gave: no stat, and a link stays a link.
                entry.dir = e.symlink_status(ec).type() == fs::file_type::directory;
                entry.name = std::move(name);
                entries.push_back(std::move(entry));
            }
            std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
                return a.key != b.key ? a.key < b.key : a.qname < b.qname;
            });
            const std::string prefix = dir == "/" ? dir : dir + "/";
            for (const Entry& e : entries) {
                if (stop->load())
                    return;
                if (!file_search::name_matches(e.qname.toLower().toStdString(), terms))
                    continue;
                const QFileInfo info(QFile::decodeName(QByteArray::fromStdString(prefix + e.name)));
                if (passes(lens, info))
                    batch.push_back(buildRow(lens, info, 0));
                if (batch.size() >= 500)
                    send();
            }
            // What's found so far shows soon, even while matches are few.
            if (!batch.empty() && Clock::now() - sent > std::chrono::milliseconds(100))
                send();
            for (const Entry& e : entries)
                if (e.dir)
                    self(self, prefix + e.name, depth + 1);
        };
        visit(visit, root, 0);
        send();
        QMetaObject::invokeMethod(this, [this, stop] {
            if (stop->load() || !searching_)
                return;
            searching_ = false;
            emit searchingChanged();
        }, Qt::QueuedConnection);
    });
}

void ChooserModel::appendRows(std::vector<Row> rows) {
    if (rows.empty())
        return;
    const int first = count();
    beginInsertRows({}, first, first + int(rows.size()) - 1);
    std::ranges::move(rows, std::back_inserter(rows_));
    endInsertRows();
    emit countChanged();
    applyPending();
}

void ChooserModel::setMode(const QString& mode) {
    if (mode == mode_)
        return;
    mode_ = mode;
    emit modeChanged();
}

void ChooserModel::setError(const QString& error) {
    if (error == error_)
        return;
    error_ = error;
    emit errorChanged();
}

void ChooserModel::reload() {
    if (mode_ == "recent")
        loadRecent();
    else if (mode_ == "search")
        runSearch();
    else if (!folder_.isEmpty())
        loadFolder();
}

QStringList ChooserModel::selection() const {
    QStringList out;
    for (const Row& r : rows_)
        if (r.selected)
            out << r.path;
    return out;
}

bool ChooserModel::allFolders() const {
    bool any = false;
    for (const Row& r : rows_)
        if (r.selected) {
            any = true;
            if (!r.isDir)
                return false;
        }
    return any;
}

bool ChooserModel::allFiles() const {
    bool any = false;
    for (const Row& r : rows_)
        if (r.selected) {
            any = true;
            if (r.isDir)
                return false;
        }
    return any;
}

void ChooserModel::select(int i, Pick how) {
    if (i < 0 || i >= count())
        return;
    const QStringList had = selection();
    if (!multiple_)
        how = Only;
    auto set = [this](int at, bool on) {
        Row& r = rows_[size_t(at)];
        on = on && r.sensitive;  // a greyed row can't be picked
        if (r.selected != on) {
            r.selected = on;
            emit dataChanged(index(at), index(at), {SelectedRole});
        }
    };
    if (how == Toggle) {
        set(i, !rows_[size_t(i)].selected);
        anchor_ = i;
    } else if (how == Extend && anchor_ >= 0 && anchor_ < count()) {
        const auto [from, to] = std::minmax(anchor_, i);
        for (int k = 0; k < count(); ++k)
            set(k, k >= from && k <= to);
    } else {
        for (int k = 0; k < count(); ++k)
            set(k, k == i);
        anchor_ = i;
    }
    if (selection() != had)
        emit selectionChanged();
}

void ChooserModel::selectAll() {
    if (!multiple_)
        return;
    // GTK's maybe_select: the kind being chosen, sensitive.
    const QStringList had = selection();
    const bool folders = act() == chooser::Action::SelectFolder;
    for (int k = 0; k < count(); ++k) {
        Row& r = rows_[size_t(k)];
        const bool on = r.sensitive && (mode_ != "browse" || r.isDir == folders);
        if (r.selected != on) {
            r.selected = on;
            emit dataChanged(index(k), index(k), {SelectedRole});
        }
    }
    if (selection() != had)
        emit selectionChanged();
}

void ChooserModel::clearSelection() {
    const QStringList had = selection();
    for (int k = 0; k < count(); ++k)
        if (rows_[size_t(k)].selected) {
            rows_[size_t(k)].selected = false;
            emit dataChanged(index(k), index(k), {SelectedRole});
        }
    anchor_ = -1;
    if (!had.isEmpty())
        emit selectionChanged();
}

void ChooserModel::selectPath(const QString& path) {
    const QFileInfo info(path);
    pending_ = {info.absoluteFilePath()};
    if (mode_ != "browse" || info.absolutePath() != folder_)
        setFolder(info.absolutePath());
    else
        applyPending();
}

void ChooserModel::applyPending() {
    if (pending_.isEmpty() || mode_ != "browse")
        return;
    const QStringList want = pending_;
    pending_.clear();
    for (const QString& p : want) {
        const QFileInfo info(p);
        if (info.absolutePath() != folder_)
            continue;
        int at = indexOf(p);
        if (at < 0 && info.exists()) {
            // Hidden: hidden files come on. Filtered out: the filter goes.
            if (!showHidden_ && (info.fileName().startsWith('.') || info.fileName().endsWith('~'))) {
                pending_ = want;
                setShowHidden(true);
                return;
            }
            if (!lets(info)) {
                pending_ = want;
                emit filterInTheWay();
                return;
            }
        }
        if (at >= 0) {
            select(at, multiple_ ? Toggle : Only);
            emit reveal(at);
        }
    }
}

int ChooserModel::indexOf(const QString& path) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].path == path)
            return int(i);
    return -1;
}

QVariantMap ChooserModel::row(int i) const {
    if (i < 0 || i >= count())
        return {};
    const Row& r = rows_[size_t(i)];
    return {{"path", r.path}, {"name", r.name}, {"isDir", r.isDir}, {"sensitive", r.sensitive}, {"selected", r.selected}};
}

QVariantMap ChooserModel::activate(int i) {
    if (i < 0 || i >= count())
        return {};
    const Row& r = rows_[size_t(i)];
    switch (chooser::on_activate(act(), r.isDir, r.sensitive)) {
    case chooser::Activate::Enter: {
        const QString path = r.path;
        setFolder(path);
        return {{"enter", path}};
    }
    case chooser::Activate::Answer:
        select(i);
        return {{"answer", true}};
    default:
        return {};
    }
}

void ChooserModel::setTrail(const QString& path, bool keep) {
    // Keeping it: the folder is on the trail already (a parent of where it
    // was, or back down): only which one is current changes.
    if (keep && trail_.contains(path)) {
        emit trailChanged();
        return;
    }
    QStringList t;
    for (QString p = path; !p.isEmpty(); p = parentOf(p))
        t.prepend(p);
    trail_ = t;
    emit trailChanged();
}

QVariantList ChooserModel::trail() const {
    QVariantList out;
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const bool underHome = folder_ == homePath() || folder_.startsWith(homePath() + "/") ||
                           std::ranges::any_of(trail_, [](const QString& p) { return p.startsWith(homePath() + "/"); });
    for (const QString& p : trail_) {
        const QString kind = p == "/"                           ? "root"
                             : p == homePath()                      ? "home"
                             : p == desktop && desktop != homePath() ? "desktop"
                                                                : "folder";
        const QString name = kind == "home" ? "Home" : kind == "root" ? QString() : QFileInfo(p).fileName();
        // Inside home the bar starts there (GTK's fake root); what's above is behind its arrow.
        const bool aboveHome = underHome && (p == "/" || homePath().startsWith(p + "/"));
        out.append(QVariantMap{{"name", name}, {"path", p}, {"kind", kind}, {"current", p == folder_},
                               {"hidden", QFileInfo(p).fileName().startsWith('.')}, {"aboveHome", aboveHome}});
    }
    return out;
}

void ChooserModel::goTrail(const QString& path) {
    // The folder it came from gets picked, and a hidden one shown.
    const int at = int(trail_.indexOf(path));
    if (at >= 0 && at + 1 < trail_.size()) {
        const QString child = trail_[at + 1];
        if (QFileInfo(child).fileName().startsWith('.'))
            setShowHidden(true);
        pending_ = {child};
    }
    setFolder(path);
}

void ChooserModel::up() {
    const int at = int(trail_.indexOf(folder_));
    if (at > 0)
        goTrail(trail_[at - 1]);
    else if (folder_ != "/")
        goTrail(parentOf(folder_));
}

void ChooserModel::down() {
    const int at = int(trail_.indexOf(folder_));
    if (at >= 0 && at + 1 < trail_.size())
        setFolder(trail_[at + 1]);
}

QVariantMap ChooserModel::resolve(const QString& text) const {
    const std::string h = homePath().toStdString(), base = folder_.toStdString();
    const QString path = QString::fromStdString(chooser::file_for_text(text.toStdString(), base, h));
    const QString dir = QString::fromStdString(chooser::directory_for_text(text.toStdString(), base, h));
    const QFileInfo info(path);
    const chooser::Split s = chooser::split(text.toStdString());
    return {{"path", path}, {"exists", !path.isEmpty() && info.exists()}, {"isDir", info.isDir()}, {"dir", dir},
            {"file", chooser::is_directory_shortcut(text.toStdString()) ? QString() : QString::fromStdString(s.file)},
            {"allowed", !path.isEmpty() && info.isFile() && lets(info)}};
}

QVariantList ChooserModel::completions(const QString& text) const {
    const std::string h = homePath().toStdString();
    const QString dir = QString::fromStdString(chooser::directory_for_text(text.toStdString(), folder_.toStdString(), h));
    if (dir.isEmpty())
        return {};
    const chooser::Split s = chooser::split(text.toStdString());
    const QString prefix = QString::fromStdString(s.file);
    QDir::Filters flags = QDir::AllDirs | QDir::NoDotAndDotDot;
    // Choosing a folder: only folders complete.
    if (act() != chooser::Action::SelectFolder)
        flags |= QDir::Files;
    if (showHidden_ || prefix.startsWith('.'))
        flags |= QDir::Hidden;
    QVariantList out;
    QDir d(dir);
    QFileInfoList entries = d.entryInfoList(flags, QDir::Name | QDir::DirsFirst | QDir::IgnoreCase);
    for (const QFileInfo& e : entries) {
        if (!e.fileName().startsWith(prefix))
            continue;
        if (!e.isDir() && !lets(e))
            continue;
        const QString shown = e.fileName() + (e.isDir() ? "/" : "");
        out.append(QVariantMap{{"name", shown}, {"text", QString::fromStdString(s.dir) + shown}, {"isDir", e.isDir()}});
    }
    return out;
}

QString ChooserModel::complete(const QString& text) const {
    std::vector<std::string> all;
    for (const QVariant& v : completions(text))
        all.push_back(v.toMap().value("text").toString().toStdString());
    return QString::fromStdString(chooser::common_prefix(all, text.toStdString()));
}

QString ChooserModel::home() const {
    return homePath();
}

int ChooserModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant ChooserModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= count())
        return {};
    const Row& r = rows_[size_t(index.row())];
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    switch (role) {
    case PathRole: return r.path;
    case NameRole: return r.name;
    case IsDirRole: return r.isDir;
    case SensitiveRole: return r.sensitive;
    case IconRole: return r.icon;
    case ThumbRole: return r.thumb;
    case SizeTextRole: return r.sizeText;
    case TypeRole: return r.type;
    case DateTextRole:
        return r.time ? QString::fromStdString(chooser::date_text(r.time, now, showTime_, clock24_)) : QString("Unknown");
    case TimeTextRole:
        return showTime_ && r.time ? QString::fromStdString(chooser::time_text(r.time, clock24_)) : QString();
    case LocationRole: return r.location;
    case SelectedRole: return r.selected;
    case HiddenRole: return r.hidden;
    default: return {};
    }
}

QHash<int, QByteArray> ChooserModel::roleNames() const {
    return {{PathRole, "path"},         {NameRole, "name"},         {IsDirRole, "isDir"},
            {SensitiveRole, "sensitive"}, {IconRole, "icon"},         {ThumbRole, "thumb"},
            {SizeTextRole, "sizeText"},   {TypeRole, "type"},         {DateTextRole, "dateText"},
            {TimeTextRole, "timeText"},   {LocationRole, "location"}, {SelectedRole, "selected"},
            {HiddenRole, "hidden"}};
}

} // namespace atrium
