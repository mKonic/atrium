#include "apps.hpp"

#include <QDateTime>

#include "audio.hpp"
#include "chooser_places.hpp"
#include "compositor.hpp"
#include "default_apps.hpp"
#include "desktop_entries.hpp"
#include "file_search_core.hpp"
#include "launcher_entries.hpp"
#include "mpris.hpp"
#include "list_sync.hpp"
#include "search.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QMimeDatabase>
#include <QUrl>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <functional>
#include <cmath>

namespace atrium {

namespace {

// The icon a window gave through xdg-toplevel-icon: a theme name, or a
// picture atrium saved (as a file URL, which QML images take as is).
QString window_icon(const QVariantMap& w) {
    const QString icon = w.value("icon").toString();
    return icon.startsWith('/') ? "file://" + icon : icon;
}

constexpr int kMaxResults = 8;
constexpr int kIdleApps = 6;     // with nothing typed: the most used
constexpr int kMaxWindows = 3;
constexpr int kWindowCutoff = 400;
constexpr double kAppCutoff = 150;  // loose matches in keywords are noise

std::string lowered(const QString& s) {
    return s.toLower().toStdString();
}

QString stateDir() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.local/state";
    return dir + "/atrium";
}

QString text(const QObject* o, const char* name) {
    const QVariant p = o->property(name);
    return p.typeId() == QMetaType::QStringList ? p.toStringList().join(' ') : p.toString();
}

void execute(QObject* entry) {
    Compositor::instance()->launchEntry(entry);
}

} // namespace

// --- EntryIndex --------------------------------------------------------------------

QObject* EntryIndex::byId(const QString& id) const {
    return shell::DesktopEntries::instance()->byId(id);
}

QObject* EntryIndex::forApp(const QString& appId) const {
    return shell::DesktopEntries::instance()->heuristicLookup(appId);
}

QString EntryIndex::idForApp(const QString& appId) const {
    const QObject* e = forApp(appId);
    return e ? e->property("id").toString() : appId;
}

QString EntryIndex::nameForApp(const QString& appId) const {
    const QObject* e = forApp(appId);
    const QString name = e ? e->property("name").toString() : QString();
    return name.isEmpty() ? appId : name;
}

QList<QObject*> EntryIndex::all() const {
    return shell::DesktopEntries::instance()->applications()->values();
}

// --- LauncherResults ---------------------------------------------------------------

LauncherResults::LauncherResults(QObject* parent)
    : QAbstractListModel(parent), countsFile_(stateDir() + "/launcher.json") {
    loadCounts();
    connect(Compositor::instance(), &Compositor::windowsChanged, this, [this] {
        if (!query_.trimmed().isEmpty())
            rebuild();
    });
}

void LauncherResults::setEntries(QObject* entries) {
    if (entries == index_.source())
        return;
    if (QObject* old = index_.source())
        disconnect(old, nullptr, this, nullptr);
    index_.setSource(entries);
    if (entries)
        connect(entries, SIGNAL(applicationsChanged()), this, SLOT(rebuildLater()));
    emit entriesChanged();
    rebuild();
}

void LauncherResults::setQuery(const QString& query) {
    if (query == query_)
        return;
    query_ = query;
    emit queryChanged();
    rebuild();
}

void LauncherResults::setCurrent(int current) {
    current = rows_.empty() ? 0 : std::clamp(current, 0, int(rows_.size()) - 1);
    if (current == current_)
        return;
    current_ = current;
    emit currentChanged();
}

void LauncherResults::move(int delta) {
    const int n = int(rows_.size());
    if (n > 0)
        setCurrent(((current_ + delta) % n + n) % n);
}

int LauncherResults::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant LauncherResults::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(rows_.size()))
        return {};
    const Row& r = rows_[size_t(index.row())];
    switch (role) {
    case KindRole: return r.kind;
    case TitleRole: return r.title;
    case SubtitleRole: return r.subtitle;
    case IconRole: return r.icon;
    case GlyphRole: return r.glyph;
    case AppIdRole: return r.entry ? r.entry->property("id") : QVariant();
    default: return {};
    }
}

QHash<int, QByteArray> LauncherResults::roleNames() const {
    return {{KindRole, "kind"}, {TitleRole, "title"}, {SubtitleRole, "subtitle"}, {IconRole, "icon"},
            {GlyphRole, "glyph"}, {AppIdRole, "appId"}};
}

void LauncherResults::rebuildLater() {
    rebuild();
}

void LauncherResults::rebuild() {
    const QString q = query_.trimmed();
    std::vector<Row> rows;

    if (q.startsWith('>') && !appsOnly_) {
        const QString cmd = q.mid(1).trimmed();
        if (!cmd.isEmpty())
            rows.push_back({.kind = "run", .title = cmd, .subtitle = "Run command", .glyph = "terminal", .text = cmd});
    } else {
        if (const auto sum = appsOnly_ ? std::nullopt : search::calculate(q.toStdString())) {
            const QString v = QString::fromStdString(search::format_number(*sum));
            rows.push_back({.kind = "calc", .title = v, .subtitle = q + " · Enter copies", .glyph = "calculate",
                            .text = v});
        }

        // Apps, best match first; with nothing typed, the ones used most.
        const std::string lq = lowered(q);
        struct Ranked {
            double score;
            QObject* entry;
        };
        std::vector<Ranked> ranked;
        for (QObject* e : index_.all()) {
            if (!e || e->property("noDisplay").toBool())
                continue;
            const int uses = counts_.value(e->property("id").toString()).toInt();
            double s;
            if (lq.empty()) {
                s = uses;
            } else {
                s = std::max({search::score(lq, lowered(text(e, "name"))) * 1.0,
                              search::score(lq, lowered(text(e, "genericName"))) * 0.8,
                              search::score(lq, lowered(text(e, "keywords"))) * 0.7,
                              search::score(lq, lowered(text(e, "id"))) * 0.6});
                if (s < kAppCutoff)
                    continue;
                s += std::log(uses + 1.0) * 40;
            }
            if (s > 0)
                ranked.push_back({s, e});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
        const size_t apps = lq.empty() ? kIdleApps : kMaxResults;
        for (size_t i = 0; i < ranked.size() && i < apps; ++i) {
            QObject* e = ranked[i].entry;
            QString sub = e->property("genericName").toString();
            if (sub.isEmpty())
                sub = e->property("comment").toString();
            if (sub.isEmpty())
                sub = "Application";
            rows.push_back({.kind = "app", .title = e->property("name").toString(), .subtitle = sub,
                            .icon = e->property("icon").toString(), .entry = e});
        }

        // Open windows, to jump to.
        if (!lq.empty() && !appsOnly_) {
            struct Hit {
                int score;
                QVariantMap w;
                QString app;
            };
            std::vector<Hit> hits;
            for (const QVariant& v : Compositor::instance()->windows()) {
                const QVariantMap w = v.toMap();
                if (w.value("skip_taskbar").toBool())
                    continue;
                const QString app = index_.nameForApp(w.value("app_id").toString());
                const int s = std::max(search::score(lq, lowered(w.value("title").toString())),
                                       search::score(lq, lowered(app)));
                if (s >= kWindowCutoff)
                    hits.push_back({s, w, app});
            }
            std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
            for (size_t i = 0; i < hits.size() && i < kMaxWindows; ++i) {
                const Hit& h = hits[i];
                const QString title = h.w.value("title").toString();
                const QObject* e = index_.forApp(h.w.value("app_id").toString());
                rows.push_back({.kind = "window", .title = title.isEmpty() ? h.app : title,
                                .subtitle = QString("Switch to %1 · Space %2").arg(h.app, h.w.value("space").toString()),
                                .icon = e ? e->property("icon").toString()
                                          : !window_icon(h.w).isEmpty() ? window_icon(h.w) : h.w.value("app_id").toString(),
                                .window = h.w.value("id").toInt()});
            }
        }
    }
    if (rows.size() > size_t(kMaxResults))
        rows.resize(kMaxResults);

    const bool counted = rows.size() != rows_.size();
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    if (counted)
        emit countChanged();
    current_ = -1;
    setCurrent(0);
}

bool LauncherResults::activate(int row) {
    if (row < 0)
        row = current_;
    if (row < 0 || row >= int(rows_.size()))
        return false;
    const Row r = rows_[size_t(row)];
    if (r.kind == "run") {
        QProcess::startDetached("sh", {"-c", r.text});
    } else if (r.kind == "calc") {
        Compositor::instance()->clipboard("set", {{"text", r.text}});
    } else if (r.kind == "app") {
        if (!r.entry)
            return false;
        remember(r.entry->property("id").toString());
        execute(r.entry);
    } else if (r.kind == "window") {
        Compositor::instance()->focusWindow(r.window);
    }
    return true;
}

void LauncherResults::loadCounts() {
    QFile f(countsFile_);
    if (f.open(QIODevice::ReadOnly))
        counts_ = QJsonDocument::fromJson(f.readAll()).toVariant().toMap();
}

void LauncherResults::remember(const QString& id) {
    counts_[id] = counts_.value(id).toInt() + 1;
    QDir().mkpath(QFileInfo(countsFile_).path());
    QSaveFile f(countsFile_);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument::fromVariant(counts_).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

// --- DockApps ----------------------------------------------------------------------

DockApps::DockApps(QObject* parent) : QAbstractListModel(parent) {
    connect(Compositor::instance(), &Compositor::windowsChanged, this, &DockApps::rebuild);
    connect(Compositor::instance(), &Compositor::appsChanged, this, &DockApps::rebuild);
    sweep_.setSingleShot(true);
    connect(&sweep_, &QTimer::timeout, this, &DockApps::rebuild);
    connect(LauncherEntries::instance(), &LauncherEntries::changed, this, &DockApps::rebuild);
}

void DockApps::setEntries(QObject* entries) {
    if (entries == index_.source())
        return;
    if (QObject* old = index_.source())
        disconnect(old, nullptr, this, nullptr);
    index_.setSource(entries);
    if (entries)
        connect(entries, SIGNAL(applicationsChanged()), this, SLOT(rebuildLater()));
    emit entriesChanged();
    rebuild();
}

int DockApps::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(apps_.size());
}

QVariant DockApps::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(apps_.size()))
        return {};
    const App& a = apps_[size_t(index.row())];
    switch (role) {
    case AppIdRole: return a.id;
    case NameRole: return a.name;
    case IconRole: return a.icon;
    case PinnedRole: return a.pinned;
    case RunningRole: return !a.windows.isEmpty();
    case FocusedRole: return a.focused;
    case WindowCountRole: return int(a.windows.size());
    case DividerRole: return a.divider;
    case LeavingRole: return a.leaving;
    case UrgentRole: return a.urgent;
    case BadgeRole: return a.badge;
    case ProgressRole: return a.progress;
    case FixedRole: return a.fixed;
    default: return {};
    }
}

QHash<int, QByteArray> DockApps::roleNames() const {
    return {{AppIdRole, "appId"}, {NameRole, "name"}, {IconRole, "icon"}, {PinnedRole, "pinned"},
            {RunningRole, "running"}, {FocusedRole, "focused"}, {WindowCountRole, "windowCount"},
            {DividerRole, "divider"}, {LeavingRole, "leaving"}, {UrgentRole, "urgent"}, {BadgeRole, "badge"}, {ProgressRole, "progress"}, {FixedRole, "fixed"}};
}

void DockApps::rebuildLater() {
    rebuild();
}

void DockApps::rebuild() {
    if (!index_.source())
        return;
    std::vector<App> next;
    const auto add = [&](const QString& id, bool pinned) -> App& {
        const QObject* e = index_.byId(id);
        App a{.id = id, .pinned = pinned};
        a.name = e ? e->property("name").toString() : id;
        a.icon = e ? e->property("icon").toString() : id;
        if (a.name.isEmpty())
            a.name = id;
        if (a.icon.isEmpty())
            a.icon = id;
        return next.emplace_back(std::move(a));
    };
    // The file manager comes first and stays, as Finder does on a Mac.
    fixed_ = DefaultApps::defaultFor("inode/directory");
    if (!index_.byId(fixed_))
        fixed_.clear();
    if (!fixed_.isEmpty())
        add(fixed_, false).fixed = true;
    for (const QString& id : Compositor::instance()->dockPins())
        if (index_.byId(id) && std::ranges::none_of(next, [&](const App& a) { return a.id == id; }))
            add(id, true);
    const size_t pinned = next.size();
    for (const QVariant& v : Compositor::instance()->windows()) {
        const QVariantMap w = v.toMap();
        if (w.value("skip_taskbar").toBool())
            continue;  // asked to stay out of the Dock
        const QString id = index_.idForApp(w.value("app_id").toString());
        auto it = std::ranges::find_if(next, [&](const App& a) { return a.id == id; });
        App& a = it != next.end() ? *it : add(id, false);
        if (!index_.byId(id))
            if (const QString icon = window_icon(w); !icon.isEmpty())
                a.icon = icon;  // no desktop entry: what the window says
        a.windows.push_back(w.value("id").toInt());
        if (const qint64 pid = w.value("pid").toLongLong(); pid > 0 && !a.pids.contains(pid))
            a.pids.push_back(pid);
        a.focused = a.focused || w.value("focused").toBool();
        a.urgent = a.urgent || w.value("urgent").toBool();
    }
    // What apps say about themselves: a count, progress, a call for attention.
    for (App& a : next) {
        const LauncherEntries::Entry e = LauncherEntries::instance()->entry(a.id);
        a.badge = e.countVisible ? e.count : -1;
        a.progress = e.progressVisible ? e.progress : -1;
        a.urgent = a.urgent || e.urgent;
    }
    // Apps that went away linger as leaving for a moment, where they were.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const App& a : next)
        leavingSince_.remove(a.id);  // back again
    bool lingering = false;
    for (size_t i = 0; i < apps_.size(); ++i) {
        const App& old = apps_[i];
        if (std::ranges::any_of(next, [&](const App& a) { return a.id == old.id; }))
            continue;
        const qint64 since = leavingSince_.value(old.id, now);
        if (now - since >= kLeaveMs) {
            leavingSince_.remove(old.id);
            continue;
        }
        leavingSince_.insert(old.id, since);
        App gone = old;
        gone.leaving = true;
        gone.focused = gone.urgent = gone.divider = false;
        gone.badge = gone.progress = -1;
        gone.windows.clear();
        gone.pids.clear();
        // After whatever came before it, if that is still here.
        size_t at = 0;
        if (i > 0)
            for (size_t j = 0; j < next.size(); ++j)
                if (next[j].id == apps_[i - 1].id)
                    at = j + 1;
        next.insert(next.begin() + std::min(at, next.size()), std::move(gone));
        lingering = true;
    }
    if (lingering)
        sweep_.start(kLeaveMs + 20);
    if (next.size() > pinned && pinned > 0 && !next[pinned].leaving)
        next[pinned].divider = true;

    struct Ops {
        DockApps* m;
        void insert(int i, const std::function<void()>& f) { m->beginInsertRows({}, i, i); f(); m->endInsertRows(); }
        void remove(int i, const std::function<void()>& f) { m->beginRemoveRows({}, i, i); f(); m->endRemoveRows(); }
        void move(int from, int to, const std::function<void()>& f) {
            m->beginMoveRows({}, from, from, {}, to);  // to < from always
            f();
            m->endMoveRows();
        }
        void change(int i, const std::function<void()>& f) {
            f();
            emit m->dataChanged(m->index(i), m->index(i));
        }
    } ops{this};
    const size_t before = apps_.size();
    sync_list(apps_, next, [](const App& a) { return a.id; }, ops);
    if (apps_.size() != before)
        emit countChanged();
}

const DockApps::App* DockApps::find(const QString& id) const {
    auto it = std::ranges::find_if(apps_, [&](const App& a) { return a.id == id; });
    return it == apps_.end() ? nullptr : &*it;
}

bool DockApps::activate(const QString& appId) {
    const App* a = find(appId);
    if (!a || a->windows.isEmpty()) {
        launch(appId);
        return true;
    }
    // Already in front: step to its next window, as Cmd+` does.
    Compositor::instance()->focusWindow(a->focused && a->windows.size() > 1 ? a->windows.back() : a->windows.front());
    return false;
}

namespace {

// A process's parent, from /proc.
qint64 parent_pid(qint64 pid) {
    QFile f(QString("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray stat = f.readAll();
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses.
    const qsizetype close = stat.lastIndexOf(')');
    if (close < 0)
        return 0;
    const QList<QByteArray> rest = stat.mid(close + 2).split(' ');
    return rest.size() > 1 ? rest[1].toLongLong() : 0;
}

QString home_relative(const QString& path) {
    const QString home = QDir::homePath();
    return path.startsWith(home + '/') ? "~" + path.mid(home.size()) : path;
}

} // namespace

QList<QObject*> DockApps::streamsOf(const App& a) const {
    // By who plays it: one of the app's processes, or a child of one
    // (Chromium and Electron play from a helper process); else by its id
    // or name, as KDE Plasma's task manager matches them.
    QList<QObject*> out;
    for (QObject* o : Audio::instance()->apps()) {
        auto* n = static_cast<AudioNode*>(o);
        bool mine = false;
        for (qint64 pid = n->pid(), hops = 0; pid > 1 && hops < 4 && !mine; pid = parent_pid(pid), ++hops)
            mine = a.pids.contains(pid);
        if (!mine && !n->appId().isEmpty())
            mine = n->appId().compare(a.id, Qt::CaseInsensitive) == 0;
        if (!mine && a.pids.isEmpty())
            mine = n->label().compare(a.name, Qt::CaseInsensitive) == 0;
        if (mine)
            out.push_back(n);
    }
    return out;
}

QObject* DockApps::playerOf(const App& a) const {
    for (QObject* o : Mpris::instance()->players()) {
        auto* p = static_cast<MprisPlayer*>(o);
        const QString entry = p->desktopEntry();
        if ((!entry.isEmpty() && index_.idForApp(entry) == a.id) || p->identity().compare(a.name, Qt::CaseInsensitive) == 0)
            return p;
    }
    return nullptr;
}

QVariantList DockApps::menuExtras(const QString& appId) {
    QVariantList rows;
    const App* a = find(appId);
    auto* entry = qobject_cast<shell::DesktopEntry*>(index_.byId(appId));
    if (!a)
        return rows;
    const auto section = [&](const QString& title) {
        if (!rows.isEmpty())
            rows.push_back(QVariantMap{{"separator", true}});
        rows.push_back(QVariantMap{{"section", title}});
    };

    // A file manager: the places it opens (KDE Plasma's Places); anything
    // else: the files it used lately (Plasma's Recent Files, from the
    // recently-used list every GTK and KDE app writes).
    if (entry && entry->categories().contains("FileManager")) {
        if (!places_)
            places_ = new ChooserPlaces(this);
        QVariantList found;
        for (const QVariant& v : places_->places()) {
            const QVariantMap p = v.toMap();
            const QString path = p.value("path").toString();
            if (path.startsWith('/') && p.value("section") != "devices")
                found.push_back(QVariantMap{{"key", "place:" + path}, {"text", p.value("name")}, {"glyph", p.value("icon")}});
        }
        if (!found.isEmpty()) {
            section(tr("Places"));
            rows.append(found.mid(0, 8));
        }
    } else if (entry) {
        QFile f(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel");
        if (f.open(QIODevice::ReadOnly)) {
            std::vector<std::string> names{entry->id().toStdString(), entry->name().toStdString(),
                                           entry->startupClass().toStdString()};
            const QStringList cmd = entry->command();
            const std::string binary = cmd.isEmpty() ? std::string() : QFileInfo(cmd.first()).fileName().toStdString();
            QVariantList found;
            for (const auto& r : file_search::recent_for_app(f.readAll().toStdString(), names, binary)) {
                const QString path = QString::fromStdString(r.path);
                if (!QFileInfo::exists(path))
                    continue;
                const bool dir = QFileInfo(path).isDir();
                found.push_back(QVariantMap{{"key", "recent:" + path}, {"text", QFileInfo(path).fileName()},
                                            {"tooltip", home_relative(path)},
                                            {"glyph", dir ? "folder" : "description"}});
                if (found.size() == 5)
                    break;
            }
            if (!found.isEmpty()) {
                section(tr("Recent Files"));
                rows.append(found);
                rows.push_back(QVariantMap{{"key", "forget-recent"}, {"text", tr("Forget Recent Files")}, {"glyph", "history_toggle_off"}});
            }
        }
    }

    // What the app's desktop file offers (New Private Window...).
    if (entry && !entry->actions().isEmpty()) {
        section(tr("Actions"));
        const QList<QObject*> actions = entry->actions();
        for (int i = 0; i < actions.size(); ++i) {
            auto* act = static_cast<shell::DesktopAction*>(actions[i]);
            rows.push_back(QVariantMap{{"key", QString("action:%1").arg(i)}, {"text", act->name()},
                                       {"themeIcon", act->icon().isEmpty() ? entry->icon() : act->icon()}});
        }
    }

    // Its player and its sound, side by side (Plasma keeps them together).
    auto* player = static_cast<MprisPlayer*>(playerOf(*a));
    const QList<QObject*> streams = streamsOf(*a);
    if (player || !streams.isEmpty()) {
        if (!rows.isEmpty())
            rows.push_back(QVariantMap{{"separator", true}});
        if (player)
            rows.push_back(QVariantMap{{"media", true}, {"playing", player->isPlaying()},
                                       {"canPrevious", player->canGoPrevious()}, {"canNext", player->canGoNext()},
                                       {"canToggle", player->canTogglePlaying()}});
        if (!streams.isEmpty()) {
            const bool muted = std::ranges::all_of(streams, [](QObject* o) { return static_cast<AudioNode*>(o)->muted(); });
            rows.push_back(QVariantMap{{"key", "mute"}, {"text", tr("Mute")}, {"glyph", muted ? "volume_off" : "volume_up"},
                                       {"checkable", true}, {"checked", muted}});
        }
    }
    if (!rows.isEmpty())
        rows.push_back(QVariantMap{{"separator", true}});
    return rows;
}

void DockApps::menuPick(const QString& appId, const QString& key) {
    const App* a = find(appId);
    auto* entry = qobject_cast<shell::DesktopEntry*>(index_.byId(appId));
    if (key.startsWith("place:") || key.startsWith("recent:")) {
        const QString path = key.section(':', 1);
        if (entry && (key.startsWith("place:") || QFileInfo(path).isDir() ||
                      entry->mimeTypes().contains(QMimeDatabase().mimeTypeForFile(path).name())))
            entry->open({QUrl::fromLocalFile(path).toString()});
        else
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));  // its default app, as Plasma falls back to
    } else if (key.startsWith("action:") && entry) {
        const int i = key.section(':', 1).toInt();
        if (i >= 0 && i < entry->actions().size())
            static_cast<shell::DesktopAction*>(entry->actions()[i])->execute();
    } else if (key == "forget-recent" && entry) {
        forgetRecent(entry);
    } else if (key == "mute" && a) {
        const QList<QObject*> streams = streamsOf(*a);
        const bool muted = std::ranges::all_of(streams, [](QObject* o) { return static_cast<AudioNode*>(o)->muted(); });
        for (QObject* o : streams)
            static_cast<AudioNode*>(o)->setMuted(!muted);
    } else if (key.startsWith("media:") && a) {
        if (auto* p = static_cast<MprisPlayer*>(playerOf(*a))) {
            if (key == "media:previous")
                p->previous();
            else if (key == "media:next")
                p->next();
            else
                p->togglePlaying();
        }
    }
}

void DockApps::forgetRecent(QObject* o) {
    auto* entry = static_cast<shell::DesktopEntry*>(o);
    const QString file = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel";
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return;
    const std::string before = in.readAll().toStdString();
    in.close();
    const QStringList cmd = entry->command();
    const std::string after = file_search::forget_app(
        before, {entry->id().toStdString(), entry->name().toStdString(), entry->startupClass().toStdString()},
        cmd.isEmpty() ? std::string() : QFileInfo(cmd.first()).fileName().toStdString());
    if (after == before)
        return;
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(after.data(), qint64(after.size()));
        out.commit();
    }
}

bool DockApps::pinDropped(const QList<QUrl>& urls, int index) {
    QStringList ids;
    for (const QUrl& url : urls) {
        const QString path = url.toLocalFile();
        if (!path.endsWith(".desktop"))
            return false;
        const QString id = QFileInfo(path).completeBaseName();
        if (!index_.byId(id))
            return false;  // not an installed app (or one hidden from menus)
        ids << id;
    }
    if (ids.isEmpty())
        return false;
    for (const QString& id : ids)
        placePin(id, index < 0 ? pinnedCount() : index++);
    return true;
}

QList<QUrl> DockApps::uriList(const QString& text) const {
    QList<QUrl> out;
    for (const QString& line : text.split('\n')) {
        const QString t = line.trimmed();
        if (!t.isEmpty() && !t.startsWith('#'))
            out << QUrl(t);
    }
    return out;
}

void DockApps::openWith(const QString& appId, const QList<QUrl>& urls) {
    auto* entry = qobject_cast<shell::DesktopEntry*>(index_.byId(appId));
    if (!entry || urls.isEmpty())
        return;
    QStringList targets;
    for (const QUrl& url : urls)
        targets << url.toString();
    entry->open(targets);
}

void DockApps::springOpen(const QString& appId) {
    if (const App* a = find(appId); a && !a->windows.isEmpty() && !a->focused)
        Compositor::instance()->focusWindow(a->windows.front());
}

void DockApps::cycle(const QString& appId, int step) {
    const App* a = find(appId);
    if (!a || a->windows.isEmpty())
        return;
    // Most recently used first: the one at the back is next, as Cmd+`
    // steps; the one after the front is the one before.
    if (!a->focused || a->windows.size() == 1)
        Compositor::instance()->focusWindow(a->windows.front());
    else
        Compositor::instance()->focusWindow(step > 0 ? a->windows.back() : a->windows.at(1));
}

void DockApps::reportIcons(QQuickItem* row, const QString& output) {
    if (!row || output.isEmpty())
        return;
    QVariantList icons;
    for (QQuickItem* item : row->childItems()) {
        const App* a = find(item->property("appId").toString());
        if (!a || a->windows.isEmpty() || !item->isVisible())
            continue;
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        QVariantList windows;
        for (int id : a->windows)
            windows.push_back(id);
        icons.push_back(QVariantMap{{"x", qRound(r.x())}, {"y", qRound(r.y())}, {"width", qRound(r.width())},
                                    {"height", qRound(r.height())}, {"windows", windows}});
    }
    if (icons == reported_ && output == reported_output_)
        return;
    reported_ = icons;
    reported_output_ = output;
    Compositor::instance()->setDockIcons(output, icons);
}

void DockApps::launch(const QString& appId) {
    execute(index_.byId(appId));
}

QString DockApps::otherGpu() const {
    return shell::DesktopEntries::instance()->otherGpu();
}

void DockApps::launchOnOtherGpu(const QString& appId) {
    if (QObject* entry = index_.byId(appId))
        Compositor::instance()->launchEntry(entry, true);
}

void DockApps::setPinned(const QString& appId, bool pinned) {
    Compositor::instance()->setPinned(appId, pinned);
}

void DockApps::placePin(const QString& appId, int index) {
    QStringList list = Compositor::instance()->dockPins();
    const qsizetype from = list.indexOf(appId);
    const QStringList before = list;
    if (from >= 0)
        list.removeAt(from);
    // Indices count the pins the Dock shows; pins of uninstalled apps keep
    // their place relative to the rest.
    qsizetype at = list.size();
    int shown = 0;
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (shown == index) {
            at = i;
            break;
        }
        if (index_.byId(list[i]) && list[i] != fixed_)  // the file manager isn't counted: it is always first
            ++shown;
    }
    list.insert(at, appId);
    if (list != before)
        Compositor::instance()->setDock(list);
}

int DockApps::pinnedCount() const {
    return int(std::ranges::count_if(apps_, [](const App& a) { return a.pinned; }));
}

void DockApps::expose(const QString& appId) {
    // By one of its windows: a Dock entry's id needn't be the windows' app id.
    if (const App* a = find(appId); a && !a->windows.isEmpty())
        Compositor::instance()->action("app-expose", QString("window:%1").arg(a->windows.first()));
}

void DockApps::closeAll(const QString& appId) {
    if (const App* a = find(appId))
        for (int id : a->windows)
            Compositor::instance()->closeWindow(id);
}

} // namespace atrium
