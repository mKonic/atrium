#include "apps.hpp"

#include "compositor.hpp"
#include "list_sync.hpp"
#include "search.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <functional>
#include <cmath>

namespace atrium {

namespace {

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
    if (entry)
        QMetaObject::invokeMethod(entry, "execute");
}

} // namespace

// --- EntryIndex --------------------------------------------------------------------

QObject* EntryIndex::call(const char* method, const QString& arg) const {
    if (!entries_)
        return nullptr;
    const QMetaObject* mo = entries_->metaObject();
    const int i = mo->indexOfMethod(QByteArray(method) + "(QString)");
    if (i < 0)
        return nullptr;
    QObject* out = nullptr;
    void* args[] = {&out, const_cast<QString*>(&arg)};
    // The return type is Quickshell's DesktopEntry*, which only its own
    // metatype matches: call through the meta-object directly.
    entries_->qt_metacall(QMetaObject::InvokeMetaMethod, i, args);
    return out;
}

QObject* EntryIndex::byId(const QString& id) const {
    return call("byId", id);
}

QObject* EntryIndex::forApp(const QString& appId) const {
    return call("heuristicLookup", appId);
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
    if (!entries_)
        return {};
    const QObject* apps = entries_->property("applications").value<QObject*>();
    return apps ? apps->property("values").value<QObjectList>() : QList<QObject*>{};
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
    default: return {};
    }
}

QHash<int, QByteArray> LauncherResults::roleNames() const {
    return {{KindRole, "kind"}, {TitleRole, "title"}, {SubtitleRole, "subtitle"}, {IconRole, "icon"},
            {GlyphRole, "glyph"}};
}

void LauncherResults::rebuildLater() {
    rebuild();
}

void LauncherResults::rebuild() {
    const QString q = query_.trimmed();
    std::vector<Row> rows;

    if (q.startsWith('>')) {
        const QString cmd = q.mid(1).trimmed();
        if (!cmd.isEmpty())
            rows.push_back({.kind = "run", .title = cmd, .subtitle = "Run command", .glyph = "terminal", .text = cmd});
    } else {
        if (const auto sum = search::calculate(q.toStdString())) {
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
        if (!lq.empty()) {
            struct Hit {
                int score;
                QVariantMap w;
                QString app;
            };
            std::vector<Hit> hits;
            for (const QVariant& v : Compositor::instance()->windows()) {
                const QVariantMap w = v.toMap();
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
                                .icon = e ? e->property("icon").toString() : h.w.value("app_id").toString(),
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
        QProcess::startDetached("wl-copy", {"--", r.text});
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
    connect(Compositor::instance(), &Compositor::settingsChanged, this, &DockApps::rebuild);
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
    default: return {};
    }
}

QHash<int, QByteArray> DockApps::roleNames() const {
    return {{AppIdRole, "appId"}, {NameRole, "name"}, {IconRole, "icon"}, {PinnedRole, "pinned"},
            {RunningRole, "running"}, {FocusedRole, "focused"}, {WindowCountRole, "windowCount"},
            {DividerRole, "divider"}};
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
    for (const QString& id : Compositor::instance()->setting("dock.pinned", QStringList{}).toStringList())
        if (index_.byId(id) && std::ranges::none_of(next, [&](const App& a) { return a.id == id; }))
            add(id, true);
    const size_t pinned = next.size();
    for (const QVariant& v : Compositor::instance()->windows()) {
        const QVariantMap w = v.toMap();
        const QString id = index_.idForApp(w.value("app_id").toString());
        auto it = std::ranges::find_if(next, [&](const App& a) { return a.id == id; });
        App& a = it != next.end() ? *it : add(id, false);
        a.windows.push_back(w.value("id").toInt());
        a.focused = a.focused || w.value("focused").toBool();
    }
    if (next.size() > pinned && pinned > 0)
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

void DockApps::launch(const QString& appId) {
    execute(index_.byId(appId));
}

void DockApps::setPinned(const QString& appId, bool pinned) {
    QStringList list = Compositor::instance()->setting("dock.pinned", QStringList{}).toStringList();
    if (pinned == list.contains(appId))
        return;
    if (pinned)
        list.push_back(appId);
    else
        list.removeAll(appId);
    Compositor::instance()->setSetting("dock.pinned", list);
}

void DockApps::closeAll(const QString& appId) {
    if (const App* a = find(appId))
        for (int id : a->windows)
            Compositor::instance()->closeWindow(id);
}

} // namespace atrium
