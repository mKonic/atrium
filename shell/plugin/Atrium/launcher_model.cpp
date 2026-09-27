#include "launcher_model.hpp"

#include "calc.hpp"
#include "calc_rates.hpp"
#include "compositor.hpp"
#include "desktop_entries.hpp"
#include "launcher_catalog.hpp"
#include "placeholders.hpp"
#include "settings_pages.hpp"
#include "window_layouts.hpp"

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace atrium {

namespace {

struct KindInfo {
    const char* id;
    const char* section;  // its header, and the word that lists it all
    const char* label;    // one of it
    int priority;         // ties: applications first
    bool hideable;        // "Hide from Search" (Settings can take it back)
};

// In section order.
constexpr KindInfo kKinds[] = {
    {"app", "Applications", "Application", 4, true},
    {"settings", "Settings", "Settings Page", 1, true},
    {"quicklink", "Quicklinks", "Quicklink", 2, false},
    {"snippet", "Snippets", "Snippet", 3, false},
    {"system", "System", "System Action", 3, true},
    {"window", "Window Management", "Window Command", 3, true},
    {"script", "Custom Commands", "Custom Command", 3, false},
    {"command", "Commands", "Command", 3, true},
    {"open-window", "Windows", "Window", 3, false},
};

constexpr int kMaxFavoriteSlots = 10;
constexpr int kMaxSuggestions = 5;
// Offered as suggestions while fewer than five things have been used.
const char* const kSuggested[] = {"command:screen:clipboard", "command:screen:files", "command:screen:emoji",
                                  "command:create-quicklink", "command:create-snippet"};

const KindInfo& info(int kind) {
    return kKinds[kind];
}

void shellAction(const QString& name) {
    Compositor::instance()->action("shell", name);
}

QString tilde(QString path) {
    if (path == "~" || path.startsWith("~/"))
        path.replace(0, 1, QDir::homePath());
    return path;
}

QString firstLine(const QString& s) {
    const QString line = s.section('\n', 0, 0).simplified();
    return line.size() > 80 ? line.left(79) + "…" : line;
}

// "github.com", "https://x.org/a", "localhost:8080": something to open in a browser.
bool looksLikeAddress(const QString& q) {
    if (q.contains(' ') || q.size() < 4)
        return false;
    static const QRegularExpression scheme("^(https?|ftp)://\\S+$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression host("^(localhost|[a-z0-9-]+(\\.[a-z0-9-]+)*\\.[a-z]{2,})(:\\d+)?(/\\S*)?$",
                                         QRegularExpression::CaseInsensitiveOption);
    return scheme.match(q).hasMatch() || host.match(q).hasMatch();
}

QString shellProgram() {
    const QString s = qEnvironmentVariable("SHELL");
    return s.isEmpty() ? QStringLiteral("/bin/sh") : s;
}

} // namespace

const char* LauncherModel::kindId(Kind k) {
    return info(int(k)).id;
}

LauncherModel::LauncherModel(QObject* parent)
    : QAbstractListModel(parent), pages_(std::make_unique<SettingsPages>()) {
    rebuildTimer_.setSingleShot(true);
    rebuildTimer_.setInterval(0);
    connect(&rebuildTimer_, &QTimer::timeout, this, [this] {
        rebuildEntries();
        rebuildRows(true);
    });
    Compositor* c = Compositor::instance();
    connect(c, &Compositor::windowsChanged, this, [this] { rebuildEntriesLater(); });
    connect(c, &Compositor::shortcutsChanged, this, [this] { rebuildEntriesLater(); });
    connect(c, &Compositor::settingsChanged, this, [this] { rebuildEntriesLater(); });
    connect(c, &Compositor::recordsChanged, this, [this](const QString&) { rebuildEntriesLater(); });
    connect(pages_.get(), &SettingsPages::changed, this, [this] { rebuildEntriesLater(); });
    // Rates arriving answer a money sum that was silent a moment ago.
    connect(CurrencyRates::instance(), &CurrencyRates::changed, this, [this] {
        if (screen_ == "root" && !query_.trimmed().isEmpty())
            rebuildRows(true);
    });
    loadCalcHistory();
}

LauncherModel::~LauncherModel() = default;

void LauncherModel::setEntries(QObject* entries) {
    if (entries == index_.source())
        return;
    if (QObject* old = index_.source())
        disconnect(old, nullptr, this, nullptr);
    index_.setSource(entries);
    if (entries)
        connect(entries, SIGNAL(applicationsChanged()), &rebuildTimer_, SLOT(start()));
    emit entriesChanged();
    rebuildEntriesLater();
}

void LauncherModel::rebuildEntriesLater() {
    rebuildTimer_.start();
}

void LauncherModel::setQuery(const QString& query) {
    // One line: a pasted break becomes a space.
    QString q = query;
    q.replace('\n', ' ');
    if (q == query_)
        return;
    query_ = q;
    emit queryChanged();
    if (listScreen())
        rebuildRows();
}

bool LauncherModel::listScreen() const {
    return screen_ == "root" || screen_ == "calculator" || screenKind().has_value();
}

std::optional<LauncherModel::Kind> LauncherModel::screenKind() const {
    if (screen_ == "windows")
        return Kind::OpenWindow;
    if (screen_ == "snippets")
        return Kind::Snippet;
    if (screen_ == "quicklinks")
        return Kind::Quicklink;
    return std::nullopt;
}

void LauncherModel::setCurrent(int current) {
    current = rows_.empty() ? 0 : std::clamp(current, 0, int(rows_.size()) - 1);
    if (current == current_)
        return;
    current_ = current;
    const Row* r = current_ < int(rows_.size()) ? &rows_[size_t(current_)] : nullptr;
    selectedKey_ = r && r->entry >= 0 ? entries_[size_t(r->entry)].key : QString();
    emit currentChanged();
}

void LauncherModel::move(int delta) {
    const int n = int(rows_.size());
    if (n > 0)
        setCurrent(((current_ + delta) % n + n) % n);
}

int LauncherModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant LauncherModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= int(rows_.size()))
        return {};
    const Row& r = rows_[size_t(index.row())];
    const Entry* e = r.entry >= 0 ? &entries_[size_t(r.entry)] : nullptr;
    switch (role) {
    case KindRole: return r.kind;
    case KeyRole: return e ? e->key : QString();
    case SectionRole: return r.section;
    case TitleRole: return r.title;
    case DetailRole: return r.detail;
    case IconRole: return r.icon;
    case GlyphRole: return r.glyph;
    case ColorRole: return e ? e->color : QString();
    case LabelRole: return r.label;
    case AliasRole: {
        const Prefs* p = e ? prefs(e->key) : nullptr;
        return p ? p->alias : QString();
    }
    case KeysRole: return e ? keysFor(e->key) : QString();
    case SlotRole: return r.slot;
    case RunningRole: return e && running(*e);
    case BadgeRole: return r.badge;
    default: return {};
    }
}

QHash<int, QByteArray> LauncherModel::roleNames() const {
    return {{KindRole, "kind"},     {KeyRole, "key"},     {SectionRole, "section"}, {TitleRole, "title"},
            {DetailRole, "detail"}, {IconRole, "icon"},   {GlyphRole, "glyph"},     {ColorRole, "color"},
            {LabelRole, "label"},   {AliasRole, "alias"}, {KeysRole, "keys"},       {SlotRole, "slot"},
            {RunningRole, "running"}, {BadgeRole, "badge"}};
}

const LauncherModel::Prefs* LauncherModel::prefs(const QString& key) const {
    auto it = prefs_.constFind(key);
    return it == prefs_.constEnd() ? nullptr : &*it;
}

QString LauncherModel::keysFor(const QString& key) const {
    return keys_.value(key);
}

bool LauncherModel::running(const Entry& e) const {
    return e.kind == Kind::App && runningApps_.contains(e.target);
}

// --- sources -------------------------------------------------------------------------

void LauncherModel::rebuildEntries() {
    Compositor* c = Compositor::instance();

    prefs_.clear();
    for (const QVariant& v : c->records("launcher_entries")) {
        const QVariantMap m = v.toMap();
        Prefs p;
        p.alias = m.value("alias").toString();
        if (!m.value("favorite").isNull() && m.value("favorite").isValid())
            p.favorite = m.value("favorite").toInt();
        p.hidden = m.value("hidden").toBool();
        prefs_.insert(m.value("key").toString(), p);
    }
    keys_.clear();
    for (const QVariant& v : c->shortcuts()) {
        const QVariantMap m = v.toMap();
        const QString arg = m.value("arg").toString();
        if (m.value("action") == "shell" && arg.startsWith("run:") && !m.value("keys").toString().isEmpty())
            keys_.insert(arg.mid(4), m.value("keys").toString());
    }
    runningApps_.clear();
    for (const QVariant& v : c->windows())
        runningApps_.insert(index_.idForApp(v.toMap().value("app_id").toString()));

    entries_.clear();
    byKey_.clear();
    auto add = [this](Entry e, const QStringList& alternates, const QStringList& keywords, const QString& subtitle) {
        e.fields.title = prepareText(e.title);
        for (const QString& a : alternates)
            if (!a.isEmpty() && a.compare(e.title, Qt::CaseInsensitive) != 0)
                e.fields.alternates.push_back(prepareText(a));
        for (const QString& k : keywords)
            if (!k.isEmpty())
                e.fields.keywords.push_back(prepareText(k));
        e.fields.subtitle = prepareText(subtitle);
        if (const Prefs* p = prefs(e.key))
            e.fields.alias = foldText(p->alias);
        byKey_.insert(e.key, int(entries_.size()));
        entries_.push_back(std::move(e));
    };

    // Applications.
    for (QObject* o : index_.all()) {
        auto* d = qobject_cast<shell::DesktopEntry*>(o);
        if (!d || d->noDisplay())
            continue;
        QStringList keywords = d->keywords();
        keywords << d->genericName();
        // What it's run as and its reverse-DNS name's last part: "code", "dolphin".
        if (const QStringList argv = d->command(); !argv.isEmpty())
            keywords << QFileInfo(argv.first()).fileName();
        QString id = d->id();
        id.remove(QRegularExpression("\\.desktop$"));
        keywords << id.section('.', -1);
        const QString generic = d->genericName().compare(d->name(), Qt::CaseInsensitive) == 0 ? QString() : d->genericName();
        add({.key = "app:" + d->id(), .kind = Kind::App, .title = d->name(), .detail = generic,
             .icon = d->icon().isEmpty() ? QStringLiteral("application-x-executable") : d->icon(),
             .target = d->id()},
            {}, keywords, {});
    }

    // Settings pages, found by the settings on them too.
    const QVariantMap byPage = pages_->byPage();
    for (const QVariant& v : pages_->pages()) {
        const QVariantMap p = v.toMap();
        const QString name = p.value("name").toString();
        QStringList keywords;
        for (const QVariant& s : byPage.value(name).toList())
            keywords << s.toMap().value("title").toString();
        add({.key = "settings:" + name, .kind = Kind::Settings, .title = name, .glyph = p.value("icon").toString(),
             .color = p.value("color").toString(), .target = name},
            {}, keywords, {});
    }

    for (const QVariant& v : c->records("quicklinks")) {
        const QVariantMap m = v.toMap();
        const QString id = m.value("id").toString();
        add({.key = "quicklink:" + id, .kind = Kind::Quicklink, .title = m.value("name").toString(),
             .detail = m.value("url").toString(), .icon = m.value("icon").toString(), .glyph = "link", .target = id,
             .inRoot = m.value("root").toBool()},
            {}, {}, {});
    }
    for (const QVariant& v : c->records("snippets")) {
        const QVariantMap m = v.toMap();
        const QString id = m.value("id").toString();
        // A snippet's keyword is one more name for it.
        add({.key = "snippet:" + id, .kind = Kind::Snippet, .title = m.value("name").toString(),
             .detail = firstLine(m.value("text").toString()), .glyph = "text_snippet", .target = id},
            {m.value("keyword").toString()}, {}, {});
    }
    for (const CatalogItem& i : systemActions())
        add({.key = QString("system:") + i.id, .kind = Kind::System, .title = i.title, .glyph = i.glyph,
             .target = i.id, .confirm = i.confirm},
            {}, QString(i.keywords).split(' ', Qt::SkipEmptyParts), {});
    for (const CatalogItem& i : windowCommands())
        add({.key = QString("window:") + i.id, .kind = Kind::Window, .title = i.title, .glyph = i.glyph,
             .target = i.id},
            {}, QString(i.keywords).split(' ', Qt::SkipEmptyParts), {});
    for (const QVariant& v : c->records("window_sizes")) {
        const QVariantMap m = v.toMap();
        const double w = m.value("width").toDouble(), h = m.value("height").toDouble();
        add({.key = "window:size:" + m.value("id").toString(), .kind = Kind::Window, .title = m.value("name").toString(),
             .detail = QString("%1% × %2%").arg(qRound(w * 100)).arg(qRound(h * 100)), .glyph = "aspect_ratio",
             .target = QString("size:%1x%2").arg(w).arg(h)},
            {}, {"size"}, {});
    }
    // Window layouts: arrangements put back in one go.
    for (const QVariant& v : WindowLayouts::instance()->layouts()) {
        const QVariantMap m = v.toMap();
        const QString name = m.value("name").toString();
        const int n = m.value("windows").toInt();
        add({.key = "window:layout:" + name, .kind = Kind::Window, .title = name,
             .detail = n == 1 ? QString("1 window") : QString("%1 windows").arg(n), .glyph = "dashboard",
             .target = "layout:" + name},
            {}, {"layout"}, {});
    }
    for (const QVariant& v : c->records("commands")) {
        const QVariantMap m = v.toMap();
        const QString id = m.value("id").toString();
        const QString icon = m.value("icon").toString();
        add({.key = "script:" + id, .kind = Kind::Script, .title = m.value("name").toString(),
             .detail = firstLine(m.value("command").toString()), .icon = icon.contains('/') || icon.contains('.') ? icon : QString(),
             .glyph = icon.isEmpty() || icon.contains('/') ? QStringLiteral("terminal") : icon, .target = id,
             .confirm = m.value("confirm").toBool()},
            {}, {}, {});
    }
    for (const CatalogItem& i : paletteCommands())
        add({.key = QString("command:") + i.id, .kind = Kind::Command, .title = i.title, .glyph = i.glyph,
             .target = i.id, .confirm = i.confirm},
            {}, QString(i.keywords).split(' ', Qt::SkipEmptyParts), {});

    // Open windows, to jump to (only for a typed query).
    for (const QVariant& v : c->windows()) {
        const QVariantMap w = v.toMap();
        if (w.value("skip_taskbar").toBool())
            continue;
        const QString appId = w.value("app_id").toString();
        const QString app = index_.nameForApp(appId);
        const QObject* d = index_.forApp(appId);
        QString icon = d ? d->property("icon").toString() : w.value("icon").toString();
        if (icon.startsWith('/'))
            icon = "file://" + icon;
        const QString title = w.value("title").toString();
        const QString space = w.value("space").toString();
        add({.key = "open-window:" + w.value("id").toString(), .kind = Kind::OpenWindow,
             .title = title.isEmpty() ? app : title,
             .detail = space.isEmpty() ? app : QString("%1 · Space %2").arg(app, space),
             .icon = icon.isEmpty() ? appId : icon, .target = w.value("id").toString()},
            {}, {}, app);
    }
}

// --- rows ------------------------------------------------------------------------------

void LauncherModel::pushEntryRow(std::vector<Row>& rows, int entry, const QString& section, int slot) const {
    const Entry& e = entries_[size_t(entry)];
    rows.push_back({.kind = kindId(e.kind), .entry = entry, .section = section, .title = e.title,
                    .detail = e.detail, .icon = e.icon, .glyph = e.glyph, .label = info(int(e.kind)).label,
                    .slot = slot});
}

void LauncherModel::rebuildRows(bool keepSelection) {
    // Captured before entries_ may have been rebuilt under the old rows: by key.
    const QString keepKey = keepSelection ? selectedKey_ : QString();
    const int keepRow = keepSelection ? current_ : -1;
    Compositor* c = Compositor::instance();
    const QString sens = c->setting("launcher.sensitivity", "medium").toString();
    const rank::Sensitivity sensitivity = sens == "low"    ? rank::Sensitivity::Low
                                          : sens == "high" ? rank::Sensitivity::High
                                                           : rank::Sensitivity::Medium;
    const double now = RankingStore::now();
    const QString trimmed = query_.trimmed();
    const std::u32string q = foldText(trimmed);
    std::vector<Row> rows;

    auto hidden = [this](const Entry& e) {
        const Prefs* p = prefs(e.key);
        return !e.inRoot || (p && p->hidden);
    };
    auto idleSort = [&](std::vector<int>& list) {
        std::vector<rank::Facts> facts(entries_.size());
        for (int i : list) {
            const Entry& e = entries_[size_t(i)];
            facts[size_t(i)] = rank::idle(e.fields, ranking_.visit(e.key), now, info(int(e.kind)).priority);
        }
        std::ranges::stable_sort(list, [&](int a, int b) { return rank::before_idle(facts[size_t(a)], facts[size_t(b)]); });
    };

    if (screen_ == "calculator") {
        // Calculator History, newest first, narrowed by what's typed.
        for (const QVariant& v : calcHistory_) {
            const QVariantMap m = v.toMap();
            if (!trimmed.isEmpty() && !m.value("input").toString().contains(trimmed, Qt::CaseInsensitive) &&
                !m.value("result").toString().contains(trimmed, Qt::CaseInsensitive))
                continue;
            rows.push_back({.kind = "calc", .title = m.value("result").toString(), .detail = m.value("input").toString(),
                            .glyph = "calculate", .label = m.value("label").toString(),
                            .target = m.value("copy").toString(), .badge = m.value("badge").toString()});
        }
    } else if (const auto only = screenKind()) {
        // One kind's own screen: all of it, ranked when something is typed.
        std::vector<int> list;
        std::vector<rank::Facts> facts(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.kind != *only)
                continue;
            if (q.empty()) {
                facts[i] = rank::idle(e.fields, ranking_.visit(e.key), now, 0);
                list.push_back(int(i));
            } else if (auto f = rank::match(q, e.fields, ranking_.visit(e.key), now, sensitivity, 0)) {
                facts[i] = *f;
                list.push_back(int(i));
            }
        }
        std::ranges::stable_sort(list, [&](int a, int b) {
            return q.empty() ? rank::before_idle(facts[size_t(a)], facts[size_t(b)])
                             : rank::before(facts[size_t(a)], facts[size_t(b)]);
        });
        for (int i : list)
            pushEntryRow(rows, i, "");
    } else if (q.empty()) {
        // Favorites, in their order.
        std::vector<std::pair<int, int>> favorites;
        for (size_t i = 0; i < entries_.size(); ++i)
            if (const Prefs* p = prefs(entries_[i].key); p && p->favorite && !p->hidden)
                favorites.emplace_back(*p->favorite, int(i));
        std::ranges::sort(favorites);
        QSet<int> placed;
        int slot = 0;
        for (const auto& [_, i] : favorites) {
            ++slot;
            pushEntryRow(rows, i, "Favorites", slot <= kMaxFavoriteSlots ? slot : 0);
            placed.insert(i);
        }
        // Suggestions: what you use that has no quicker way in, then a few commands worth knowing.
        if (c->setting("launcher.suggestions", true).toBool()) {
            std::vector<int> used;
            for (size_t i = 0; i < entries_.size(); ++i) {
                const Entry& e = entries_[i];
                const rank::Visit* v = ranking_.visit(e.key);
                if (!placed.contains(int(i)) && !hidden(e) && e.kind != Kind::OpenWindow && v &&
                    rank::frecency(*v, now) > 1 && keysFor(e.key).isEmpty())
                    used.push_back(int(i));
            }
            idleSort(used);
            if (used.size() > size_t(kMaxSuggestions))
                used.resize(kMaxSuggestions);
            for (const char* key : kSuggested) {
                if (used.size() >= size_t(kMaxSuggestions))
                    break;
                const int i = byKey_.value(key, -1);
                const Prefs* p = i >= 0 ? prefs(key) : nullptr;
                if (i >= 0 && !placed.contains(i) && std::ranges::find(used, i) == used.end() &&
                    keysFor(key).isEmpty() && !(p && (p->hidden || !p->alias.isEmpty())))
                    used.push_back(i);
            }
            for (int i : used) {
                pushEntryRow(rows, i, "Suggestions");
                placed.insert(i);
            }
        }
        for (int k = 0; k < int(std::size(kKinds)); ++k) {
            if (Kind(k) == Kind::OpenWindow)
                continue;
            std::vector<int> list;
            for (size_t i = 0; i < entries_.size(); ++i)
                if (int(entries_[i].kind) == k && !placed.contains(int(i)) && !hidden(entries_[i]))
                    list.push_back(int(i));
            idleSort(list);
            for (int i : list)
                pushEntryRow(rows, i, info(k).section);
        }
    } else if (trimmed.startsWith('>')) {
        const QString cmd = trimmed.mid(1).trimmed();
        if (!cmd.isEmpty())
            rows.push_back({.kind = "fallback", .title = cmd, .detail = "Run in Shell", .glyph = "terminal",
                            .label = "Shell", .target = "shell"});
    } else {
        // An inline answer first: a sum, a conversion, a date, or an address to open.
        calc::Context context;
        context.zone = QString::fromUtf8(QTimeZone::systemTimeZoneId()).toStdString();
        context.rates = CurrencyRates::instance()->rates();
        if (const QString home = QLocale().currencySymbol(QLocale::CurrencyIsoCode); home.size() == 3)
            context.home_currency = home.toStdString();
        if (const auto a = calc::evaluate(trimmed.toStdString(), context))
            rows.push_back({.kind = "calc", .title = QString::fromStdString(a->result),
                            .detail = QString::fromStdString(a->input), .glyph = "calculate",
                            .label = QString::fromStdString(a->result_badge), .target = QString::fromStdString(a->copy),
                            .badge = QString::fromStdString(a->input_badge)});
        if (looksLikeAddress(trimmed))
            rows.push_back({.kind = "url", .title = "Open in Browser", .detail = trimmed, .glyph = "open_in_browser",
                            .label = "Command", .target = trimmed});

        // A category's own name lists all of it.
        int category = -1;
        for (int k = 0; k < int(std::size(kKinds)); ++k)
            if (q == foldText(info(k).section) || q == foldText(info(k).label))
                category = k;
        if (category >= 0) {
            std::vector<int> list;
            for (size_t i = 0; i < entries_.size(); ++i)
                if (!hidden(entries_[i]) && (int(entries_[i].kind) == category || entries_[i].fields.title.folded == q))
                    list.push_back(int(i));
            idleSort(list);
            std::ranges::stable_partition(list, [&](int i) { return int(entries_[size_t(i)].kind) != category; });
            for (int i : list)
                pushEntryRow(rows, i, int(entries_[size_t(i)].kind) == category ? info(category).section : "");
        } else {
            struct Hit {
                int entry;
                rank::Facts facts;
            };
            std::vector<Hit> hits;
            for (size_t i = 0; i < entries_.size(); ++i) {
                const Entry& e = entries_[i];
                if (hidden(e))
                    continue;
                if (auto f = rank::match(q, e.fields, ranking_.visit(e.key), now, sensitivity, info(int(e.kind)).priority))
                    hits.push_back({int(i), *f});
            }
            std::ranges::stable_sort(hits, [](const Hit& a, const Hit& b) { return rank::before(a.facts, b.facts); });
            for (const Hit& h : hits)
                pushEntryRow(rows, h.entry, "");
        }

        // What nothing recognised goes to: under "Use “…” with".
        const QString with = QString("Use “%1” with").arg(trimmed.size() > 24 ? trimmed.left(23) + "…" : trimmed);
        rows.push_back({.kind = "fallback", .section = with, .title = "Search Files", .glyph = "folder_open",
                        .label = "Command", .target = "files"});
        for (const QVariant& v : c->records("quicklinks")) {
            const QVariantMap m = v.toMap();
            if (!placeholders::arguments(m.value("url").toString().toStdString()).empty())
                rows.push_back({.kind = "fallback", .section = with, .title = m.value("name").toString(),
                                .icon = m.value("icon").toString(), .glyph = "link", .label = "Quicklink",
                                .target = "quicklink:" + m.value("id").toString()});
        }
        rows.push_back({.kind = "fallback", .section = with, .title = "Run in Shell", .glyph = "terminal",
                        .label = "Command", .target = "shell"});
    }

    const bool counted = rows.size() != rows_.size();
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    if (counted)
        emit countChanged();
    // A new query lands on the first row; anything else (a favorite added,
    // a window opening) keeps the highlight on the entry it was on.
    const int keep = keepKey.isEmpty() ? -1 : rowForKey(keepKey);
    current_ = -1;
    setCurrent(keep >= 0 ? keep : keepRow >= 0 ? keepRow : 0);
}

void LauncherModel::open(const QString& screen, const QString& query) {
    stack_.clear();
    screen_ = screen.isEmpty() ? QStringLiteral("root") : screen;
    emit screenChanged();
    query_ = query;
    emit queryChanged();
    rebuildEntries();
    rebuildRows();
    CurrencyRates::instance()->refresh();
}

void LauncherModel::push(const QString& screen, const QString& query) {
    // The root search is the root: going there drops the stack instead of piling on it.
    if (screen == "root") {
        open("root", query);
        return;
    }
    stack_.push_back({screen_, query_, current_});
    screen_ = screen;
    query_ = query;
    emit queryChanged();
    emit screenChanged();
    if (listScreen())
        rebuildRows();
}

bool LauncherModel::escape() {
    if (!query_.isEmpty()) {
        setQuery({});
        return true;
    }
    return backspace();
}

bool LauncherModel::backspace() {
    if (stack_.empty())
        return false;
    const Frame f = stack_.back();
    stack_.pop_back();
    screen_ = f.screen;
    query_ = f.query;
    emit queryChanged();
    emit screenChanged();
    if (listScreen()) {
        rebuildRows();
        setCurrent(f.current);
    }
    return true;
}

void LauncherModel::tab() {
    // A ring of two: the query narrows either list, so it comes along.
    const QString q = query_;
    if (screen_ == "root")
        push("clipboard", q);
    else if (screen_ == "clipboard" && !stack_.empty() && stack_.back().screen == "root")
        backspace(), setQuery(q);
    else if (screen_ == "clipboard")
        open("root", q);
}

const LauncherModel::Entry* LauncherModel::entryAt(int row) const {
    if (row < 0)
        row = current_;
    if (row < 0 || row >= int(rows_.size()) || rows_[size_t(row)].entry < 0)
        return nullptr;
    return &entries_[size_t(rows_[size_t(row)].entry)];
}

int LauncherModel::rowForKey(const QString& key) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].entry >= 0 && entries_[size_t(rows_[i].entry)].key == key)
            return int(i);
    return -1;
}

QVariantMap LauncherModel::record(const QString& table, const QString& id) const {
    for (const QVariant& v : Compositor::instance()->records(table))
        if (v.toMap().value("id").toString() == id)
            return v.toMap();
    return {};
}

QVariantList LauncherModel::arguments() const {
    const Entry* e = entryAt(-1);
    QString text;
    if (e && e->kind == Kind::Quicklink)
        text = record("quicklinks", e->target).value("url").toString();
    else if (e && e->kind == Kind::Snippet)
        text = record("snippets", e->target).value("text").toString();
    else if (e && e->kind == Kind::Script)
        text = record("commands", e->target).value("command").toString();
    QVariantList out;
    for (const placeholders::Argument& a : placeholders::arguments(text.toStdString()))
        out.push_back(QVariantMap{{"name", QString::fromStdString(a.name)}, {"optional", a.fallback.has_value()}});
    return out;
}

// --- running things ----------------------------------------------------------------------

void LauncherModel::activate(int row, const QVariantList& args) {
    if (row < 0)
        row = current_;
    if (row < 0 || row >= int(rows_.size()))
        return;
    const Row r = rows_[size_t(row)];
    if (r.entry >= 0) {
        const Entry& e = entries_[size_t(r.entry)];
        if (persistent(e)) {
            // A category word is not a search for the row that ran.
            const bool category = std::ranges::any_of(kKinds, [&](const KindInfo& k) {
                return foldText(query_) == foldText(k.section) || foldText(query_) == foldText(k.label);
            });
            ranking_.record(e.key, category ? QString() : query_);
        }
        const Entry copy = e;  // running it may rebuild entries_
        runEntry(copy, args, true);
        return;
    }
    runRow(r, args);
}

void LauncherModel::runRow(const Row& r, const QVariantList&) {
    if (r.kind == "calc") {
        QGuiApplication::clipboard()->setText(r.target);
        rememberCalc(r);
        emit closeRequested();
        emit feedback("content_copy", "Copied " + r.title, false);
    } else if (r.kind == "url") {
        emit closeRequested();
        QDesktopServices::openUrl(QUrl::fromUserInput(r.target));
    } else if (r.kind == "fallback") {
        const QString q = query_.trimmed().startsWith('>') ? query_.trimmed().mid(1).trimmed() : query_.trimmed();
        if (r.target == "files") {
            push("files", q);
        } else if (r.target == "shell") {
            runCustom({{"name", q}, {"command", q}, {"output", true}}, {});
        } else if (r.target.startsWith("quicklink:")) {
            openQuicklink(record("quicklinks", r.target.mid(10)), {q});
        }
    }
}

void LauncherModel::runEntry(const Entry& e, const QVariantList& args, bool fromPalette) {
    if (e.confirm) {
        emit confirmRequested(e.title + "?", e.kind == Kind::System ? "This can't be undone." : QString(), e.glyph,
                              "run|" + e.key);
        return;
    }
    switch (e.kind) {
    case Kind::App: {
        emit closeRequested();
        // Running: to its most recent window, as the Dock does; else start it.
        if (running(e)) {
            for (const QVariant& v : Compositor::instance()->windows()) {
                const QVariantMap w = v.toMap();
                if (index_.idForApp(w.value("app_id").toString()) == e.target && !w.value("skip_taskbar").toBool()) {
                    Compositor::instance()->focusWindow(w.value("id").toInt());
                    return;
                }
            }
        }
        if (QObject* d = index_.byId(e.target))
            QMetaObject::invokeMethod(d, "execute");
        return;
    }
    case Kind::Settings:
        emit closeRequested();
        shellAction("settings:" + e.target);
        return;
    case Kind::Quicklink:
        openQuicklink(record("quicklinks", e.target), args);
        return;
    case Kind::Snippet:
        typeSnippet(record("snippets", e.target), args, false);
        return;
    case Kind::System: {
        emit closeRequested();
        bool noop = false;
        const QString said = runSystemAction(e.target, &noop);
        if (!said.isEmpty())
            emit feedback(e.glyph, said, noop);
        return;
    }
    case Kind::Window:
        emit closeRequested();
        if (e.target.startsWith("layout:"))
            WindowLayouts::instance()->run(e.target.mid(7), index_);
        else if (e.target == "save-layout") {
            const QString name = WindowLayouts::instance()->capture();
            emit feedback("dashboard_customize", name.isEmpty() ? QStringLiteral("No Windows to Save")
                                                                : QString("Saved as “%1”").arg(name),
                          name.isEmpty());
        } else
            Compositor::instance()->action("place", e.target);
        return;
    case Kind::Script:
        runCustom(record("commands", e.target), args);
        return;
    case Kind::Command:
        runCommand(e.target);
        return;
    case Kind::OpenWindow:
        emit closeRequested();
        Compositor::instance()->focusWindow(e.target.toInt());
        return;
    }
    Q_UNUSED(fromPalette);
}

void LauncherModel::runCommand(const QString& id) {
    if (id.startsWith("screen:")) {
        push(id.mid(7));
        return;
    }
    emit closeRequested();
    if (id == "create-quicklink" || id == "create-snippet" || id == "create-command")
        shellAction("settings:Launcher/" + id.mid(7));
    else if (id == "settings")
        shellAction("settings");
    else if (id == "launcher-settings")
        shellAction("settings:Launcher");
    else if (id == "screenshot" || id == "record" || id == "notifications" || id == "control" || id == "welcome")
        shellAction(id);
    else if (id == "notes")
        shellAction("notes");
    else if (id == "new-note")
        shellAction("notes:new");
    else if (id == "overview")
        Compositor::instance()->action("overview");
    else if (id == "reset-ranking") {
        ranking_.resetAll();
        emit feedback("restart_alt", "Learned Ranking Reset", false);
    }
}

QString LauncherModel::expand(const QString& text, const QVariantList& args, int encoding) const {
    placeholders::Values v;
    for (const QVariant& a : args)
        v.arguments.push_back(a.toString().toStdString());
    v.clipboard = QGuiApplication::clipboard()->text().toStdString();
    const QDateTime now = QDateTime::currentDateTime();
    const QLocale locale;
    v.date = locale.toString(now.date(), QLocale::ShortFormat).toStdString();
    v.time = locale.toString(now.time(), QLocale::ShortFormat).toStdString();
    v.datetime = locale.toString(now, QLocale::ShortFormat).toStdString();
    v.uuid = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); };
    return QString::fromStdString(
        placeholders::expand(text.toStdString(), v, placeholders::Encoding(encoding)));
}

void LauncherModel::openQuicklink(const QVariantMap& m, const QVariantList& args) {
    if (m.isEmpty())
        return;
    const QString raw = m.value("url").toString().trimmed();
    // Web addresses take their values URL-encoded; paths and the rest as typed.
    const bool web = raw.contains("://") && !raw.startsWith("file://");
    QString url = expand(raw, args, int(web ? placeholders::Encoding::Url : placeholders::Encoding::Plain));
    url = tilde(url);
    emit closeRequested();
    const QString app = m.value("app").toString();
    if (!app.isEmpty())
        if (auto* d = qobject_cast<shell::DesktopEntry*>(index_.byId(app))) {
            d->open({url});
            return;
        }
    QDesktopServices::openUrl(url.startsWith('/') ? QUrl::fromLocalFile(url) : QUrl::fromUserInput(url));
}

void LauncherModel::typeSnippet(const QVariantMap& m, const QVariantList& args, bool copyOnly) {
    if (m.isEmpty())
        return;
    const QString text = expand(m.value("text").toString(), args, int(placeholders::Encoding::Plain));
    emit closeRequested();
    if (copyOnly) {
        QGuiApplication::clipboard()->setText(text);
        emit feedback("content_copy", "Snippet Copied", false);
        return;
    }
    // Typed into the field the palette was opened over (copied when none takes it).
    Compositor::instance()->insertText(text);
}

void LauncherModel::expandSnippet(qint64 snippet, int before) {
    const QVariantMap m = record("snippets", QString::number(snippet));
    if (m.isEmpty())
        return;
    // Arguments can't be asked for mid-typing: their defaults, else nothing.
    const QString text = expand(m.value("text").toString(), {}, int(placeholders::Encoding::Plain));
    Compositor::instance()->replaceText(before, text);
}

void LauncherModel::runCustom(const QVariantMap& m, const QVariantList& args) {
    if (m.isEmpty())
        return;
    const QString name = m.value("name").toString();
    const QString cmd = expand(m.value("command").toString(), args, int(placeholders::Encoding::Shell));
    QString dir = tilde(m.value("directory").toString());
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        dir = QDir::homePath();
    const QStringList argv{shellProgram(), "-c", cmd};

    if (m.value("terminal").toBool()) {
        emit closeRequested();
        QStringList t = shell::DesktopEntries::inTerminal(argv);
        if (t.isEmpty()) {
            emit feedback("error", "No Terminal Installed", true);
            return;
        }
        QProcess p;
        p.setProgram(t.takeFirst());
        p.setArguments(t);
        p.setWorkingDirectory(dir);
        p.startDetached();
        return;
    }

    if (m.value("output").toBool()) {
        if (process_) {
            process_->disconnect(this);
            process_->kill();
            process_->waitForFinished(500);
        }
        process_ = std::make_unique<QProcess>();
        outputTitle_ = name;
        output_.clear();
        outputExit_ = 0;
        outputRecord_ = m;
        outputArgs_ = args;
        process_->setProgram(argv[0]);
        process_->setArguments(argv.mid(1));
        process_->setWorkingDirectory(dir);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        QProcess* p = process_.get();
        connect(p, &QProcess::readyRead, this, [this, p] {
            output_ += QString::fromLocal8Bit(p->readAll());
            // Colour and cursor escapes mean nothing in a plain view.
            static const QRegularExpression ansi("\x1b\\[[0-9;?]*[A-Za-z]");
            output_.remove(ansi);
            emit outputChanged();
        });
        connect(p, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            outputExit_ = status == QProcess::CrashExit ? -1 : code;
            emit outputChanged();
        });
        connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { emit outputChanged(); });
        process_->start();
        emit outputChanged();
        if (screen_ != "output")
            push("output");
        return;
    }

    // Quietly, saying when it's done.
    emit closeRequested();
    auto* p = new QProcess(this);
    p->setProgram(argv[0]);
    p->setArguments(argv.mid(1));
    p->setWorkingDirectory(dir);
    p->setProcessChannelMode(QProcess::ForwardedChannels);
    connect(p, &QProcess::finished, this, [this, p, name](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0)
            emit feedback("check_circle", QString("“%1” Finished").arg(name), false);
        else
            emit feedback("error", QString("“%1” Failed (exit %2)").arg(name).arg(code), false);
        p->deleteLater();
    });
    p->start();
}

void LauncherModel::stopOutput() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->terminate();
        if (!process_->waitForFinished(1000))
            process_->kill();
    }
}

void LauncherModel::rerunOutput() {
    if (!outputRecord_.isEmpty())
        runCustom(outputRecord_, outputArgs_);
}

void LauncherModel::copyOutput() const {
    QGuiApplication::clipboard()->setText(output_);
}

void LauncherModel::run(const QString& key) {
    rebuildEntries();
    const int i = byKey_.value(key, -1);
    if (i < 0)
        return;
    const Entry e = entries_[size_t(i)];
    // Something that asks for arguments opens the palette on it.
    const bool asks = (e.kind == Kind::Quicklink || e.kind == Kind::Snippet || e.kind == Kind::Script) && [&] {
        const QString text = e.kind == Kind::Quicklink ? record("quicklinks", e.target).value("url").toString()
                             : e.kind == Kind::Snippet ? record("snippets", e.target).value("text").toString()
                                                       : record("commands", e.target).value("command").toString();
        const auto a = placeholders::arguments(text.toStdString());
        return std::ranges::any_of(a, [](const placeholders::Argument& x) { return !x.fallback; });
    }();
    if (asks) {
        emit openRequested(e.title);
        return;
    }
    // An app's own shortcut toggles it: in front, it goes out of the way.
    if (e.kind == Kind::App && running(e)) {
        const QVariant focused = Compositor::instance()->focusedWindow();
        if (focused.isValid() && index_.idForApp(focused.toMap().value("app_id").toString()) == e.target) {
            for (const QVariant& v : Compositor::instance()->windows()) {
                const QVariantMap w = v.toMap();
                if (index_.idForApp(w.value("app_id").toString()) == e.target && !w.value("minimized").toBool())
                    Compositor::instance()->windowRequest(w.value("id").toInt(), "minimize", {{"value", true}});
            }
            return;
        }
    }
    runEntry(e, {}, false);
}

void LauncherModel::confirm(const QString& token) {
    const QString verb = token.section('|', 0, 0), key = token.section('|', 1);
    const int i = byKey_.value(key, -1);
    if (i < 0)
        return;
    Entry e = entries_[size_t(i)];
    if (verb == "run") {
        e.confirm = false;
        runEntry(e, {}, true);
    } else if (verb == "uninstall") {
        const Uninstall u = uninstallFor(e);
        if (u.trashFile.isEmpty())
            runCustom({{"name", "Uninstall " + e.title}, {"command", u.command}, {"output", true}}, {});
        else if (QFile::moveToTrash(u.trashFile))
            emit feedback("delete_forever", e.title + " Removed", false);
    } else if (verb == "delete" && e.key.startsWith("window:layout:")) {
        WindowLayouts::instance()->remove(e.target.mid(7));
        if (prefs(e.key))
            Compositor::instance()->removeRecord("launcher_entries", e.key);
        ranking_.reset(e.key);
    } else if (verb == "delete") {
        const QString table = e.kind == Kind::Quicklink ? "quicklinks"
                              : e.kind == Kind::Snippet ? "snippets"
                              : e.kind == Kind::Script  ? "commands"
                                                        : (e.key.startsWith("window:size:") ? "window_sizes" : "");
        if (table.isEmpty())
            return;
        Compositor::instance()->removeRecord(table, e.key.section(':', -1).toLongLong());
        if (prefs(e.key))
            Compositor::instance()->removeRecord("launcher_entries", e.key);
        ranking_.reset(e.key);
    }
}

// --- calculator history ------------------------------------------------------------

namespace {

QString calcHistoryFile() {
    QString dir = qEnvironmentVariable("XDG_STATE_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + "/.local/state";
    return dir + "/atrium/launcher-calculator.json";
}

} // namespace

void LauncherModel::loadCalcHistory() {
    QFile f(calcHistoryFile());
    if (f.open(QIODevice::ReadOnly))
        calcHistory_ = QJsonDocument::fromJson(f.readAll()).array().toVariantList();
}

void LauncherModel::saveCalcHistory() const {
    QDir().mkpath(QFileInfo(calcHistoryFile()).path());
    QSaveFile f(calcHistoryFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonArray::fromVariantList(calcHistory_)).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void LauncherModel::rememberCalc(const Row& r) {
    // One entry per question, the latest on top.
    calcHistory_.removeIf([&](const QVariant& v) { return v.toMap().value("input") == r.detail; });
    calcHistory_.prepend(QVariantMap{{"input", r.detail}, {"result", r.title}, {"copy", r.target},
                                     {"badge", r.badge}, {"label", r.label},
                                     {"when", QDateTime::currentSecsSinceEpoch()}});
    saveCalcHistory();
}

// --- uninstalling -----------------------------------------------------------------

// How an app came to be here, and so how it goes: a Flatpak, a package, or
// a launcher entry of the user's own (which only goes to the trash).
LauncherModel::Uninstall LauncherModel::uninstallFor(const Entry& e) const {
    Uninstall u;
    auto* d = qobject_cast<shell::DesktopEntry*>(index_.byId(e.target));
    if (!d)
        return u;
    const QString file = QFileInfo(d->file()).canonicalFilePath();
    if (file.contains("/flatpak/")) {
        const QString app = QFileInfo(file).completeBaseName();
        u.command = "flatpak uninstall --noninteractive -y " + app;
        u.says = QString("The Flatpak %1 goes; its data in ~/.var/app stays.").arg(app);
        return u;
    }
    if (file.startsWith(QDir::homePath())) {
        u.trashFile = d->file();
        u.command = "trash";
        u.says = "It's a launcher entry of your own: it goes to the Trash.";
        return u;
    }
    if (QStandardPaths::findExecutable("pacman").isEmpty())
        return u;
    QProcess owner;
    owner.start("pacman", {"-Qqo", file});
    if (!owner.waitForFinished(3000) || owner.exitCode() != 0)
        return u;
    const QString pkg = QString::fromUtf8(owner.readAllStandardOutput()).trimmed();
    if (pkg.isEmpty() || pkg.contains('\n'))
        return u;
    // -R alone: the package, not what it pulled in, and nothing that needs it.
    u.command = "pkexec pacman -R --noconfirm " + pkg;
    u.says = QString("The package %1 is removed (you'll be asked for your password). What it depends on stays.").arg(pkg);
    return u;
}

// --- preferences ---------------------------------------------------------------------

QVariantList LauncherModel::items(const QString& query) {
    if (entries_.empty())
        rebuildEntries();
    QHash<QString, qint64> shortcutIds;
    for (const QVariant& v : Compositor::instance()->shortcuts()) {
        const QVariantMap m = v.toMap();
        if (m.value("action") == "shell" && m.value("arg").toString().startsWith("run:"))
            shortcutIds.insert(m.value("arg").toString().mid(4), m.value("id").toLongLong());
    }
    const std::u32string q = foldText(query);
    std::vector<int> list;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (!persistent(e))
            continue;
        if (!q.empty()) {
            const auto sc = rank::score(q, e.fields.title);
            if (!sc || !rank::passes(*sc, rank::query_length(q), rank::Sensitivity::Medium))
                continue;
        }
        list.push_back(int(i));
    }
    std::ranges::stable_sort(list, [this](int a, int b) {
        const Entry &x = entries_[size_t(a)], &y = entries_[size_t(b)];
        if (x.kind != y.kind)
            return x.kind < y.kind;
        return rank::compare_names(x.fields.title.folded, y.fields.title.folded) < 0;
    });
    QVariantList out;
    for (int i : list) {
        const Entry& e = entries_[size_t(i)];
        const Prefs* p = prefs(e.key);
        out.push_back(QVariantMap{
            {"key", e.key},
            {"title", e.title},
            {"section", info(int(e.kind)).section},
            {"label", info(int(e.kind)).label},
            {"icon", e.icon},
            {"glyph", e.glyph},
            {"color", e.color},
            {"alias", p ? p->alias : QString()},
            {"hidden", p && p->hidden},
            {"hideable", info(int(e.kind)).hideable},
            {"keys", keysFor(e.key)},
            {"shortcut", shortcutIds.value(e.key, -1)},
        });
    }
    return out;
}

void LauncherModel::setAlias(const QString& key, const QString& alias) {
    setPref(key, {{"alias", alias.trimmed()}});
}

void LauncherModel::setHidden(const QString& key, bool hidden) {
    setPref(key, {{"hidden", hidden}});
}

QVariantList LauncherModel::windowLayouts() const {
    return WindowLayouts::instance()->layouts();
}

void LauncherModel::renameLayout(const QString& from, const QString& to) {
    WindowLayouts::instance()->rename(from, to);
}

void LauncherModel::removeLayout(const QString& name) {
    WindowLayouts::instance()->remove(name);
}

void LauncherModel::setPref(const QString& key, const QVariantMap& fields) {
    Compositor::instance()->setRecord("launcher_entries", key, fields);
}

void LauncherModel::toggleFavorite(const Entry& e) {
    const Prefs* p = prefs(e.key);
    if (p && p->favorite) {
        setPref(e.key, {{"favorite", QVariant::fromValue(nullptr)}});
        return;
    }
    int last = -1;
    for (const Prefs& o : prefs_)
        if (o.favorite)
            last = std::max(last, *o.favorite);
    setPref(e.key, {{"favorite", last + 1}});
}

void LauncherModel::moveFavorite(const Entry& e, int direction) {
    std::vector<std::pair<int, QString>> favs;
    for (auto it = prefs_.begin(); it != prefs_.end(); ++it)
        if (it->favorite && byKey_.contains(it.key()))
            favs.emplace_back(*it->favorite, it.key());
    std::ranges::sort(favs);
    const auto at = std::ranges::find_if(favs, [&](const auto& f) { return f.second == e.key; });
    if (at == favs.end())
        return;
    const auto other = at + direction;
    if (other < favs.begin() || other >= favs.end())
        return;
    // Swap the two positions; every other favorite keeps its own.
    setPref(at->second, {{"favorite", other->first}});
    setPref(other->second, {{"favorite", at->first}});
}

void LauncherModel::quitApp(const Entry& e, bool restart) {
    QList<int> ids;
    for (const QVariant& v : Compositor::instance()->windows()) {
        const QVariantMap w = v.toMap();
        if (index_.idForApp(w.value("app_id").toString()) == e.target)
            ids.push_back(w.value("id").toInt());
    }
    if (ids.isEmpty())
        return;
    emit closeRequested();
    for (int id : ids)
        Compositor::instance()->closeWindow(id);
    if (!restart)
        return;
    // Start it again once every window is gone; an app that refuses to close
    // (a save dialog left standing) is left as it is.
    auto* waiter = new QTimer(this);
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + 5000;
    const QString target = e.target;
    connect(waiter, &QTimer::timeout, this, [this, waiter, ids, until, target] {
        bool left = false;
        for (const QVariant& v : Compositor::instance()->windows())
            left = left || ids.contains(v.toMap().value("id").toInt());
        if (left && QDateTime::currentMSecsSinceEpoch() < until)
            return;
        waiter->deleteLater();
        if (!left)
            if (QObject* d = index_.byId(target))
                QMetaObject::invokeMethod(d, "execute");
    });
    waiter->start(100);
}

// --- actions menu ----------------------------------------------------------------------

QVariantList LauncherModel::actions(int row, const QString& query) const {
    QVariantList all = allActions(row);
    const std::u32string q = foldText(query);
    if (q.empty())
        return all;
    QVariantList out;
    for (const QVariant& v : all) {
        QVariantMap a = v.toMap();
        const auto sc = rank::score(q, prepareText(a.value("title").toString()));
        if (sc && rank::passes(*sc, rank::query_length(q), rank::Sensitivity::Medium)) {
            a["group"] = !out.isEmpty() && a.value("group").toBool();
            out.push_back(a);
        }
    }
    return out;
}

QVariantList LauncherModel::allActions(int row) const {
    if (row < 0)
        row = current_;
    if (row < 0 || row >= int(rows_.size()))
        return {};
    const Row& r = rows_[size_t(row)];
    QVariantList out;
    bool group = false;
    auto add = [&](const QString& id, const QString& title, const QString& glyph, const QString& keys = {}) {
        out.push_back(QVariantMap{{"id", id}, {"title", title}, {"glyph", glyph}, {"keys", keys}, {"group", group}});
        group = false;
    };
    auto section = [&] { group = !out.isEmpty(); };

    if (r.entry < 0) {
        if (r.kind == "calc") {
            add("open", "Copy Answer", "content_copy", "Enter");
            add("type", "Type Answer", "keyboard", "Ctrl+Enter");
            add("copy-expression", "Copy Question", "functions");
            if (screen_ == "calculator") {
                section();
                add("forget", "Remove from History", "delete");
                add("forget-all", "Clear History", "delete_sweep");
            }
        } else {
            add("open", r.kind == "url" ? QStringLiteral("Open in Browser") : !r.detail.isEmpty() ? r.detail : r.title, r.glyph,
                "Enter");
        }
        return out;
    }
    const Entry& e = entries_[size_t(r.entry)];
    const Prefs* p = prefs(e.key);
    switch (e.kind) {
    case Kind::App: {
        add("open", running(e) ? "Switch to App" : "Open", "open_in_new", "Enter");
        if (running(e))
            add("new-window", "New Window", "add_box");
        if (auto* d = qobject_cast<shell::DesktopEntry*>(index_.byId(e.target)))
            for (int i = 0; i < d->actions().size(); ++i)
                add("desktop-action:" + QString::number(i), d->actions()[i]->property("name").toString(), "bolt");
        section();
        const bool pinned = Compositor::instance()->dockPins().contains(e.target);
        add(pinned ? "unpin" : "pin", pinned ? "Remove from Dock" : "Keep in Dock", "dock_to_bottom");
        add("reveal", "Show Desktop File", "folder_open", "Ctrl+Enter");
        add("uninstall", "Uninstall…", "delete_forever");
        if (running(e)) {
            section();
            add("restart", "Restart App", "refresh", "Ctrl+R");
            add("quit", "Quit App", "close", "Ctrl+Shift+Q");
        }
        break;
    }
    case Kind::Quicklink:
        add("open", "Open Quicklink", "open_in_new", "Enter");
        add("copy", "Copy Address", "content_copy", "Ctrl+Shift+C");
        break;
    case Kind::Snippet:
        add("open", "Type Snippet", "keyboard", "Enter");
        add("copy", "Copy Snippet", "content_copy", "Ctrl+Shift+C");
        break;
    case Kind::Script:
        add("open", "Run Command", "play_arrow", "Enter");
        break;
    case Kind::OpenWindow:
        add("open", "Switch to Window", "select_window", "Enter");
        add("minimize", "Minimize Window", "minimize");
        add("close-window", "Close Window", "close");
        return out;
    default:
        add("open", e.kind == Kind::Settings ? "Open Settings" : "Run", e.glyph, "Enter");
        break;
    }

    section();
    const bool favorite = p && p->favorite;
    add("favorite", favorite ? "Remove from Favorites" : "Add to Favorites", favorite ? "star" : "star_outline",
        "Ctrl+Shift+F");
    if (favorite && r.section == "Favorites") {
        if (row > 0 && rows_[size_t(row - 1)].section == "Favorites")
            add("favorite-up", "Move Favorite Up", "arrow_upward", "Ctrl+Alt+Up");
        if (row + 1 < int(rows_.size()) && rows_[size_t(row + 1)].section == "Favorites")
            add("favorite-down", "Move Favorite Down", "arrow_downward", "Ctrl+Alt+Down");
    }
    if (info(int(e.kind)).hideable)
        add("hide", "Hide from Search", "visibility_off", "Ctrl+Shift+H");
    if (ranking_.learned(e.key))
        add("reset-ranking", "Reset Ranking", "restart_alt");
    section();
    add("configure", "Alias and Shortcut…", "tune");
    if (e.kind == Kind::Quicklink || e.kind == Kind::Snippet || e.kind == Kind::Script ||
        e.key.startsWith("window:size:") || e.key.startsWith("window:layout:")) {
        add("edit", "Edit…", "edit");
        section();
        add("delete", "Delete", "delete");
    }
    return out;
}

void LauncherModel::runAction(int row, const QString& id) {
    if (row < 0)
        row = current_;
    if (row < 0 || row >= int(rows_.size()))
        return;
    const Row r = rows_[size_t(row)];
    if (id == "open") {
        activate(row);
        return;
    }
    if (r.entry < 0) {
        if (r.kind == "calc" && id == "type") {
            rememberCalc(r);
            emit closeRequested();
            Compositor::instance()->insertText(r.target);
        } else if (r.kind == "calc" && id == "copy-expression") {
            QGuiApplication::clipboard()->setText(r.detail);
            emit closeRequested();
        } else if (r.kind == "calc" && (id == "forget" || id == "forget-all")) {
            if (id == "forget-all")
                calcHistory_.clear();
            else
                calcHistory_.removeIf([&](const QVariant& v) { return v.toMap().value("input") == r.detail; });
            saveCalcHistory();
            rebuildRows(true);
        }
        return;
    }
    const Entry e = entries_[size_t(r.entry)];
    Compositor* c = Compositor::instance();
    if (id == "favorite") {
        toggleFavorite(e);
    } else if (id == "favorite-up" || id == "favorite-down") {
        moveFavorite(e, id == "favorite-up" ? -1 : 1);
    } else if (id == "hide") {
        setPref(e.key, {{"hidden", true}});
    } else if (id == "reset-ranking") {
        ranking_.reset(e.key);
        rebuildRows(true);
    } else if (id == "configure") {
        emit closeRequested();
        shellAction("settings:Launcher/items");
    } else if (id == "edit") {
        emit closeRequested();
        const QString tab = e.kind == Kind::Quicklink ? "quicklinks"
                            : e.kind == Kind::Snippet ? "snippets"
                            : e.kind == Kind::Script  ? "commands"
                                                      : "sizes";
        shellAction("settings:Launcher/" + tab);
    } else if (id == "delete") {
        emit confirmRequested(QString("Delete “%1”?").arg(e.title), "This can't be undone.", "delete", "delete|" + e.key);
    } else if (id == "copy") {
        if (e.kind == Kind::Quicklink) {
            QGuiApplication::clipboard()->setText(record("quicklinks", e.target).value("url").toString());
            emit closeRequested();
        } else if (e.kind == Kind::Snippet) {
            typeSnippet(record("snippets", e.target), {}, true);
        }
    } else if (id == "new-window") {
        emit closeRequested();
        if (QObject* d = index_.byId(e.target))
            QMetaObject::invokeMethod(d, "execute");
    } else if (id.startsWith("desktop-action:")) {
        emit closeRequested();
        if (auto* d = qobject_cast<shell::DesktopEntry*>(index_.byId(e.target))) {
            const int i = id.section(':', 1).toInt();
            if (i >= 0 && i < d->actions().size())
                QMetaObject::invokeMethod(d->actions()[i], "execute");
        }
    } else if (id == "pin" || id == "unpin") {
        c->setPinned(e.target, id == "pin");
    } else if (id == "reveal") {
        if (auto* d = qobject_cast<shell::DesktopEntry*>(index_.byId(e.target))) {
            emit closeRequested();
            QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                            "org.freedesktop.FileManager1", "ShowItems");
            m << QStringList{QUrl::fromLocalFile(d->file()).toString()} << QString();
            QDBusConnection::sessionBus().asyncCall(m);
        }
    } else if (id == "uninstall") {
        const Uninstall u = uninstallFor(e);
        if (u.command.isEmpty()) {
            emit feedback("error", "Can't Tell What Installed It", true);
            return;
        }
        emit confirmRequested(QString("Uninstall %1?").arg(e.title), u.says, "delete_forever", "uninstall|" + e.key);
    } else if (id == "quit" || id == "restart") {
        quitApp(e, id == "restart");
    } else if (id == "minimize") {
        emit closeRequested();
        c->windowRequest(e.target.toInt(), "minimize");
    } else if (id == "close-window") {
        c->closeWindow(e.target.toInt());
    }
}

bool LauncherModel::chord(const QString& id) {
    if (current_ < 0 || current_ >= int(rows_.size()))
        return false;
    const QVariantList offered = allActions(current_);
    const bool has = std::ranges::any_of(offered, [&](const QVariant& a) { return a.toMap().value("id") == id; });
    if (!has)
        return false;
    runAction(current_, id);
    return true;
}

bool LauncherModel::openFavorite(int slot) {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].slot == slot && rows_[i].section == "Favorites") {
            activate(int(i));
            return true;
        }
    return false;
}

} // namespace atrium
