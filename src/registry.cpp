#include "registry.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <regex>

namespace fs = std::filesystem;

namespace atrium {

namespace {

// One prepared statement, finalized when it goes out of scope.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (db && sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            stmt_ = nullptr;
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(stmt_, i, v.c_str(), int(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, int64_t v) {
        sqlite3_bind_int64(stmt_, i, v);
        return *this;
    }
    Stmt& bind(int i, int v) { return bind(i, int64_t(v)); }
    Stmt& bind(int i, double v) {
        sqlite3_bind_double(stmt_, i, v);
        return *this;
    }
    Stmt& bind(int i, bool v) { return bind(i, int64_t(v)); }
    template <class T> Stmt& bind(int i, const std::optional<T>& v) {
        if (v)
            return bind(i, *v);
        sqlite3_bind_null(stmt_, i);
        return *this;
    }

    bool step() { return stmt_ && sqlite3_step(stmt_) == SQLITE_ROW; }
    bool run() { return stmt_ && sqlite3_step(stmt_) == SQLITE_DONE; }

    std::string text(int col) const {
        const auto* t = sqlite3_column_text(stmt_, col);
        return t ? reinterpret_cast<const char*>(t) : "";
    }
    int64_t integer(int col) const { return sqlite3_column_int64(stmt_, col); }
    double real(int col) const { return sqlite3_column_double(stmt_, col); }
    bool null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }
    std::optional<int> opt_int(int col) const { return null(col) ? std::nullopt : std::optional<int>(int(integer(col))); }
    std::optional<bool> opt_bool(int col) const { return null(col) ? std::nullopt : std::optional<bool>(integer(col) != 0); }
    std::optional<double> opt_real(int col) const { return null(col) ? std::nullopt : std::optional<double>(real(col)); }
    std::optional<std::string> opt_text(int col) const { return null(col) ? std::nullopt : std::optional<std::string>(text(col)); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

constexpr int kSchemaVersion = 8;

constexpr const char* kApps =
    "SELECT app_id, secret, space, launch, dock, maximized, fullscreen,"
    " place_output, place_x, place_y, place_w, place_h, place_maximized, place_snapped FROM apps";

AppRecord read_app(const Stmt& s) {
    AppRecord a;
    a.app_id = s.text(0);
    a.secret = s.text(1);
    a.space = int(s.integer(2));
    a.launch = s.text(3);
    a.dock = s.opt_int(4);
    a.maximized = s.opt_bool(5);
    a.fullscreen = s.opt_bool(6);
    if (!s.null(10) && s.integer(10) > 0) {
        a.placement = Placement{s.text(7), int(s.integer(8)), int(s.integer(9)), int(s.integer(10)),
                                int(s.integer(11)), s.integer(12) != 0, uint32_t(s.integer(13))};
    }
    return a;
}

constexpr const char* kRules =
    "SELECT id, app_pattern, title_pattern, secret, space, launch, maximized, fullscreen FROM rules ORDER BY position, id";

RuleRecord read_rule(const Stmt& s) {
    return RuleRecord{s.integer(0), s.text(1), s.text(2), s.text(3), int(s.integer(4)), s.text(5),
                      s.opt_bool(6), s.opt_bool(7)};
}

// "discord|^vesktop$|WhatsApp" → the plain names, or nothing when the
// pattern does more than list names.
std::optional<std::vector<std::string>> plain_names(const std::string& pattern) {
    static const std::regex name("\\^?([A-Za-z0-9._-]+)\\$?");
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= pattern.size()) {
        const size_t bar = pattern.find('|', start);
        const std::string part = pattern.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        std::smatch m;
        if (!std::regex_match(part, m, name))
            return std::nullopt;
        out.push_back(m[1]);
        if (bar == std::string::npos)
            break;
        start = bar + 1;
    }
    return out;
}

} // namespace

bool AppRecord::empty() const {
    return secret.empty() && space == 0 && launch.empty() && !dock && !maximized && !fullscreen && !placement;
}

Registry::Registry(const std::string& path) {
    if (path != ":memory:") {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        fresh_ = !fs::exists(path);
    } else {
        fresh_ = true;
    }
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_busy_timeout(db_, 2000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    migrate();
}

Registry::~Registry() {
    sqlite3_close(db_);
}

bool Registry::exec(const char* sql) const {
    return db_ && sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

void Registry::migrate() {
    Stmt v(db_, "PRAGMA user_version");
    const int version = v.step() ? int(v.integer(0)) : 0;
    if (version >= kSchemaVersion)
        return;
    exec("BEGIN");
    exec("CREATE TABLE IF NOT EXISTS settings ("
         " key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    exec("CREATE TABLE IF NOT EXISTS apps ("
         " app_id TEXT PRIMARY KEY COLLATE NOCASE,"
         " secret TEXT NOT NULL DEFAULT '', space INTEGER NOT NULL DEFAULT 0, launch TEXT NOT NULL DEFAULT '',"
         " dock INTEGER, maximized INTEGER, fullscreen INTEGER,"
         " place_output TEXT, place_x INTEGER, place_y INTEGER, place_w INTEGER, place_h INTEGER,"
         " place_maximized INTEGER, place_snapped INTEGER)");
    exec("CREATE TABLE IF NOT EXISTS rules ("
         " id INTEGER PRIMARY KEY, position INTEGER NOT NULL DEFAULT 0,"
         " app_pattern TEXT NOT NULL DEFAULT '', title_pattern TEXT NOT NULL DEFAULT '',"
         " secret TEXT NOT NULL DEFAULT '', space INTEGER NOT NULL DEFAULT 0, launch TEXT NOT NULL DEFAULT '',"
         " maximized INTEGER, fullscreen INTEGER)");
    exec("CREATE TABLE IF NOT EXISTS shortcuts ("
         " id INTEGER PRIMARY KEY, position INTEGER NOT NULL DEFAULT 0,"
         " keys TEXT NOT NULL, action TEXT NOT NULL, arg TEXT NOT NULL DEFAULT '', locked INTEGER NOT NULL DEFAULT 0)");
    exec("CREATE TABLE IF NOT EXISTS displays ("
         " id TEXT PRIMARY KEY, enabled INTEGER NOT NULL DEFAULT 1,"
         " width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, refresh INTEGER NOT NULL DEFAULT 0,"
         " scale REAL NOT NULL DEFAULT 1, transform INTEGER NOT NULL DEFAULT 0, x INTEGER, y INTEGER)");
    exec("CREATE TABLE IF NOT EXISTS devices ("
         " name TEXT PRIMARY KEY, speed REAL, acceleration TEXT, natural_scroll INTEGER, left_handed INTEGER)");
    exec("CREATE TABLE IF NOT EXISTS sessions ("
         " id TEXT PRIMARY KEY, app_id TEXT NOT NULL DEFAULT '', used INTEGER NOT NULL DEFAULT 0)");
    exec("CREATE TABLE IF NOT EXISTS session_windows ("
         " session TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, name TEXT NOT NULL,"
         " output TEXT NOT NULL DEFAULT '', x INTEGER NOT NULL DEFAULT 0, y INTEGER NOT NULL DEFAULT 0,"
         " w INTEGER NOT NULL DEFAULT 0, h INTEGER NOT NULL DEFAULT 0, maximized INTEGER NOT NULL DEFAULT 0,"
         " snapped INTEGER NOT NULL DEFAULT 0, fullscreen INTEGER NOT NULL DEFAULT 0,"
         " PRIMARY KEY(session, name))");
    // 3: tiling arrived with a shortcut of its own; registries from before
    // it get that one default (new ones are seeded with all of them).
    if (version == 2)
        exec("INSERT INTO shortcuts (position, keys, action) "
             "SELECT COALESCE(MAX(position), 0) + 1, 'Mod+backslash', 'toggle-tiling' FROM shortcuts "
             "WHERE NOT EXISTS (SELECT 1 FROM shortcuts WHERE action = 'toggle-tiling')");
    // 4: arrows as in caelestia (focus and move by direction). Old defaults
    // that were never changed become the new ones; the rest are added.
    if (version >= 2 && version < 4) {
        exec("UPDATE shortcuts SET action = 'focus-direction', arg = 'left' WHERE keys = 'Mod+Left' AND action = 'snap-left'");
        exec("UPDATE shortcuts SET action = 'focus-direction', arg = 'right' WHERE keys = 'Mod+Right' AND action = 'snap-right'");
        exec("UPDATE shortcuts SET action = 'focus-direction', arg = 'down' WHERE keys = 'Mod+Down' AND action = 'restore'");
        exec("UPDATE shortcuts SET keys = 'Mod+Alt+F' WHERE keys = 'Mod+Up' AND action = 'maximize'");
        const char* added[][3] = {
            {"Mod+Up", "focus-direction", "up"},
            {"Mod+Shift+Left", "move-direction", "left"}, {"Mod+Shift+Right", "move-direction", "right"},
            {"Mod+Shift+Up", "move-direction", "up"}, {"Mod+Shift+Down", "move-direction", "down"},
            {"Mod+Ctrl+Shift+Left", "move-to-space-prev", ""}, {"Mod+Ctrl+Shift+Right", "move-to-space-next", ""},
            {"Mod+Alt+Space", "toggle-floating", ""}, {"Mod+P", "toggle-pin", ""},
            {"Mod+Page_Up", "space-prev", ""}, {"Mod+Page_Down", "space-next", ""},
            {"Mod+0", "space", "10"}, {"Mod+Shift+0", "move-to-space", "10"},
        };
        for (const auto& a : added) {
            Stmt add(db_, "INSERT INTO shortcuts (position, keys, action, arg) "
                          "SELECT COALESCE(MAX(position), 0) + 1, ?1, ?2, ?3 FROM shortcuts "
                          "WHERE NOT EXISTS (SELECT 1 FROM shortcuts WHERE keys = ?1)");
            add.bind(1, std::string(a[0])).bind(2, std::string(a[1])).bind(3, std::string(a[2])).run();
        }
    }
    // 5: app exposé, on macOS's key for it (Ctrl+Down) plus Mod.
    if (version >= 2 && version < 5)
        exec("INSERT INTO shortcuts (position, keys, action) "
             "SELECT COALESCE(MAX(position), 0) + 1, 'Mod+Ctrl+Down', 'app-expose' FROM shortcuts "
             "WHERE NOT EXISTS (SELECT 1 FROM shortcuts WHERE keys = 'Mod+Ctrl+Down')");
    // 8: the emoji picker (Super+Period, as in caelestia).
    if (version >= 2 && version < 8)
        exec("INSERT INTO shortcuts (position, keys, action, arg) "
             "SELECT COALESCE(MAX(position), 0) + 1, 'Mod+period', 'shell', 'emoji' FROM shortcuts "
             "WHERE NOT EXISTS (SELECT 1 FROM shortcuts WHERE keys = 'Mod+period')");
    exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
    exec("COMMIT");
}

void Registry::begin() {
    exec("BEGIN");
}

void Registry::commit() {
    exec("COMMIT");
}

fs::path Registry::default_file() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return fs::path(config) / "atrium" / "registry.db";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".config" / "atrium" / "registry.db";
}

// --- settings ------------------------------------------------------------------------

std::map<std::string, json> Registry::settings() const {
    std::map<std::string, json> out;
    Stmt s(db_, "SELECT key, value FROM settings");
    while (s.step()) {
        json v = json::parse(s.text(1), nullptr, false);
        if (!v.is_discarded())
            out[s.text(0)] = std::move(v);
    }
    return out;
}

void Registry::set_setting(const std::string& key, const json& value) {
    Stmt(db_, "INSERT INTO settings(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value = ?2")
        .bind(1, key).bind(2, value.dump()).run();
}

void Registry::erase_setting(const std::string& key) {
    Stmt(db_, "DELETE FROM settings WHERE key = ?1").bind(1, key).run();
}

// --- apps ----------------------------------------------------------------------------

std::vector<AppRecord> Registry::apps() const {
    std::vector<AppRecord> out;
    Stmt s(db_, (std::string(kApps) + " ORDER BY app_id").c_str());
    while (s.step())
        out.push_back(read_app(s));
    return out;
}

std::optional<AppRecord> Registry::app(const std::string& app_id) const {
    Stmt s(db_, (std::string(kApps) + " WHERE app_id = ?1").c_str());
    s.bind(1, app_id);
    if (s.step())
        return read_app(s);
    return std::nullopt;
}

void Registry::put_app(const AppRecord& a) {
    if (a.empty()) {
        remove_app(a.app_id);
        return;
    }
    const std::optional<Placement>& p = a.placement;
    Stmt s(db_,
           "INSERT INTO apps(app_id, secret, space, launch, dock, maximized, fullscreen, place_output, place_x,"
           " place_y, place_w, place_h, place_maximized, place_snapped)"
           " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)"
           " ON CONFLICT(app_id) DO UPDATE SET secret = ?2, space = ?3, launch = ?4, dock = ?5, maximized = ?6,"
           " fullscreen = ?7, place_output = ?8, place_x = ?9, place_y = ?10, place_w = ?11, place_h = ?12,"
           " place_maximized = ?13, place_snapped = ?14");
    s.bind(1, a.app_id).bind(2, a.secret).bind(3, a.space).bind(4, a.launch).bind(5, a.dock)
        .bind(6, a.maximized).bind(7, a.fullscreen);
    if (p) {
        s.bind(8, p->output).bind(9, p->x).bind(10, p->y).bind(11, p->width).bind(12, p->height)
            .bind(13, p->maximized).bind(14, int64_t(p->snapped));
    } else {
        for (int i = 8; i <= 14; ++i)
            s.bind(i, std::optional<int>{});
    }
    s.run();
}

void Registry::remove_app(const std::string& app_id) {
    Stmt(db_, "DELETE FROM apps WHERE app_id = ?1").bind(1, app_id).run();
}

void Registry::set_dock(const std::vector<std::string>& ids) {
    begin();
    exec("UPDATE apps SET dock = NULL");
    int position = 0;
    for (const std::string& id : ids) {
        if (id.empty())
            continue;
        AppRecord a = app(id).value_or(AppRecord{.app_id = id});
        a.dock = position++;
        put_app(a);
    }
    // Apps that were only pinned are gone now.
    exec("DELETE FROM apps WHERE dock IS NULL AND secret = '' AND space = 0 AND launch = '' AND maximized IS NULL"
         " AND fullscreen IS NULL AND (place_w IS NULL OR place_w = 0)");
    commit();
}

// --- rules ---------------------------------------------------------------------------

std::vector<RuleRecord> Registry::rules() const {
    std::vector<RuleRecord> out;
    Stmt s(db_, kRules);
    while (s.step())
        out.push_back(read_rule(s));
    return out;
}

int64_t Registry::add_rule(const RuleRecord& r) {
    Stmt s(db_,
           "INSERT INTO rules(position, app_pattern, title_pattern, secret, space, launch, maximized, fullscreen)"
           " VALUES((SELECT COALESCE(MAX(position), 0) + 1 FROM rules), ?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    s.bind(1, r.app_pattern).bind(2, r.title_pattern).bind(3, r.secret).bind(4, r.space).bind(5, r.launch)
        .bind(6, r.maximized).bind(7, r.fullscreen);
    return s.run() ? sqlite3_last_insert_rowid(db_) : 0;
}

bool Registry::update_rule(const RuleRecord& r) {
    Stmt s(db_,
           "UPDATE rules SET app_pattern = ?2, title_pattern = ?3, secret = ?4, space = ?5, launch = ?6,"
           " maximized = ?7, fullscreen = ?8 WHERE id = ?1");
    s.bind(1, r.id).bind(2, r.app_pattern).bind(3, r.title_pattern).bind(4, r.secret).bind(5, r.space)
        .bind(6, r.launch).bind(7, r.maximized).bind(8, r.fullscreen);
    return s.run() && sqlite3_changes(db_) > 0;
}

bool Registry::remove_rule(int64_t id) {
    return Stmt(db_, "DELETE FROM rules WHERE id = ?1").bind(1, id).run() && sqlite3_changes(db_) > 0;
}

// --- shortcuts -----------------------------------------------------------------------

std::vector<ShortcutRecord> Registry::shortcuts() const {
    std::vector<ShortcutRecord> out;
    Stmt s(db_, "SELECT id, keys, action, arg, locked FROM shortcuts ORDER BY position, id");
    while (s.step())
        out.push_back({s.integer(0), s.text(1), s.text(2), s.text(3), s.integer(4) != 0});
    return out;
}

int64_t Registry::add_shortcut(const ShortcutRecord& k) {
    Stmt s(db_,
           "INSERT INTO shortcuts(position, keys, action, arg, locked)"
           " VALUES((SELECT COALESCE(MAX(position), 0) + 1 FROM shortcuts), ?1, ?2, ?3, ?4)");
    s.bind(1, k.keys).bind(2, k.action).bind(3, k.arg).bind(4, k.locked);
    return s.run() ? sqlite3_last_insert_rowid(db_) : 0;
}

bool Registry::update_shortcut(const ShortcutRecord& k) {
    Stmt s(db_, "UPDATE shortcuts SET keys = ?2, action = ?3, arg = ?4, locked = ?5 WHERE id = ?1");
    s.bind(1, k.id).bind(2, k.keys).bind(3, k.action).bind(4, k.arg).bind(5, k.locked);
    return s.run() && sqlite3_changes(db_) > 0;
}

bool Registry::remove_shortcut(int64_t id) {
    return Stmt(db_, "DELETE FROM shortcuts WHERE id = ?1").bind(1, id).run() && sqlite3_changes(db_) > 0;
}

void Registry::replace_shortcuts(const std::vector<ShortcutRecord>& list) {
    begin();
    exec("DELETE FROM shortcuts");
    for (const ShortcutRecord& k : list)
        add_shortcut(k);
    commit();
}

// --- displays ------------------------------------------------------------------------

std::optional<DisplayRecord> Registry::display(const std::string& id) const {
    Stmt s(db_, "SELECT id, enabled, width, height, refresh, scale, transform, x, y FROM displays WHERE id = ?1");
    s.bind(1, id);
    if (!s.step())
        return std::nullopt;
    DisplayRecord d{s.text(0), s.integer(1) != 0, int(s.integer(2)), int(s.integer(3)), int(s.integer(4)),
                    s.real(5), int(s.integer(6)), s.opt_int(7), s.opt_int(8)};
    return d;
}

void Registry::put_display(const DisplayRecord& d) {
    Stmt s(db_,
           "INSERT INTO displays(id, enabled, width, height, refresh, scale, transform, x, y)"
           " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) ON CONFLICT(id) DO UPDATE SET enabled = ?2, width = ?3,"
           " height = ?4, refresh = ?5, scale = ?6, transform = ?7, x = ?8, y = ?9");
    s.bind(1, d.id).bind(2, d.enabled).bind(3, d.width).bind(4, d.height).bind(5, d.refresh).bind(6, d.scale)
        .bind(7, d.transform).bind(8, d.x).bind(9, d.y);
    s.run();
}

// --- devices -------------------------------------------------------------------------

namespace {

DeviceRecord device_row(const Stmt& s) {
    return {s.text(0), s.opt_real(1), s.opt_text(2), s.opt_bool(3), s.opt_bool(4)};
}

} // namespace

std::optional<DeviceRecord> Registry::device(const std::string& name) const {
    Stmt s(db_, "SELECT name, speed, acceleration, natural_scroll, left_handed FROM devices WHERE name = ?1");
    s.bind(1, name);
    if (!s.step())
        return std::nullopt;
    return device_row(s);
}

std::vector<DeviceRecord> Registry::devices() const {
    std::vector<DeviceRecord> out;
    Stmt s(db_, "SELECT name, speed, acceleration, natural_scroll, left_handed FROM devices ORDER BY name");
    while (s.step())
        out.push_back(device_row(s));
    return out;
}

void Registry::put_device(const DeviceRecord& d) {
    if (!d.speed && !d.acceleration && !d.natural_scroll && !d.left_handed) {
        Stmt s(db_, "DELETE FROM devices WHERE name = ?1");
        s.bind(1, d.name).run();
        return;
    }
    Stmt s(db_,
           "INSERT INTO devices(name, speed, acceleration, natural_scroll, left_handed) VALUES(?1, ?2, ?3, ?4, ?5)"
           " ON CONFLICT(name) DO UPDATE SET speed = ?2, acceleration = ?3, natural_scroll = ?4, left_handed = ?5");
    s.bind(1, d.name).bind(2, d.speed).bind(3, d.acceleration).bind(4, d.natural_scroll).bind(5, d.left_handed);
    s.run();
}

// --- sessions ------------------------------------------------------------------------

bool Registry::has_session(const std::string& id) const {
    Stmt s(db_, "SELECT 1 FROM sessions WHERE id = ?1");
    s.bind(1, id);
    return s.step();
}

void Registry::touch_session(const std::string& id, const std::string& app_id) {
    Stmt s(db_, "INSERT INTO sessions(id, app_id, used) VALUES(?1, ?2, unixepoch())"
                " ON CONFLICT(id) DO UPDATE SET used = unixepoch(), app_id = ?2");
    s.bind(1, id).bind(2, app_id).run();
}

void Registry::remove_session(const std::string& id) {
    Stmt w(db_, "DELETE FROM session_windows WHERE session = ?1");
    w.bind(1, id).run();
    Stmt s(db_, "DELETE FROM sessions WHERE id = ?1");
    s.bind(1, id).run();
}

std::optional<SessionWindow> Registry::session_window(const std::string& session, const std::string& name) const {
    Stmt s(db_, "SELECT name, output, x, y, w, h, maximized, snapped, fullscreen FROM session_windows"
                " WHERE session = ?1 AND name = ?2");
    s.bind(1, session).bind(2, name);
    if (!s.step())
        return std::nullopt;
    return SessionWindow{s.text(0),
                         Placement{s.text(1), int(s.integer(2)), int(s.integer(3)), int(s.integer(4)),
                                   int(s.integer(5)), s.integer(6) != 0, uint32_t(s.integer(7))},
                         s.integer(8) != 0};
}

void Registry::put_session_window(const std::string& session, const SessionWindow& w) {
    const Placement& p = w.placement;
    Stmt s(db_, "INSERT INTO session_windows(session, name, output, x, y, w, h, maximized, snapped, fullscreen)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) ON CONFLICT(session, name) DO UPDATE SET"
                " output = ?3, x = ?4, y = ?5, w = ?6, h = ?7, maximized = ?8, snapped = ?9, fullscreen = ?10");
    s.bind(1, session).bind(2, w.name).bind(3, p.output).bind(4, p.x).bind(5, p.y).bind(6, p.width)
        .bind(7, p.height).bind(8, p.maximized).bind(9, int64_t(p.snapped)).bind(10, w.fullscreen);
    s.run();
}

void Registry::remove_session_window(const std::string& session, const std::string& name) {
    Stmt s(db_, "DELETE FROM session_windows WHERE session = ?1 AND name = ?2");
    s.bind(1, session).bind(2, name).run();
}

void Registry::rename_session_window(const std::string& session, const std::string& from, const std::string& to) {
    Stmt s(db_, "UPDATE session_windows SET name = ?3 WHERE session = ?1 AND name = ?2");
    s.bind(1, session).bind(2, from).bind(3, to).run();
}

void Registry::drop_sessions_before(int64_t before) {
    Stmt w(db_, "DELETE FROM session_windows WHERE session IN (SELECT id FROM sessions WHERE used < ?1)");
    w.bind(1, before).run();
    Stmt s(db_, "DELETE FROM sessions WHERE used < ?1");
    s.bind(1, before).run();
}

// --- JSON forms ----------------------------------------------------------------------

std::vector<AppRecord> apps_from_legacy(const json& rules, const json& pinned, const json& placements,
                                        std::vector<RuleRecord>* leftover) {
    std::map<std::string, AppRecord> by_id;
    auto get = [&](const std::string& id) -> AppRecord& {
        for (auto& [k, a] : by_id)
            if (std::ranges::equal(k, id, [](char x, char y) { return std::tolower(x) == std::tolower(y); }))
                return a;
        AppRecord& a = by_id[id];
        a.app_id = id;
        return a;
    };
    auto opt_bool = [](const json& r, const char* key) {
        return r.contains(key) && r[key].is_boolean() ? std::optional<bool>(r[key].get<bool>()) : std::nullopt;
    };
    if (rules.is_array())
        for (const json& r : rules) {
            if (!r.is_object())
                continue;
            const std::string app = r.value("app_id", ""), title = r.value("title", "");
            const auto names = title.empty() && !app.empty() ? plain_names(app) : std::nullopt;
            if (!names) {
                if (leftover && (!app.empty() || !title.empty()))
                    leftover->push_back({0, app, title, r.value("secret", ""), r.value("space", 0),
                                         r.value("launch", ""), opt_bool(r, "maximized"), opt_bool(r, "fullscreen")});
                continue;
            }
            for (const std::string& n : *names) {
                AppRecord& a = get(n);
                a.secret = r.value("secret", a.secret);
                a.space = r.value("space", a.space);
                a.launch = r.value("launch", a.launch);
                if (auto m = opt_bool(r, "maximized"))
                    a.maximized = m;
                if (auto f = opt_bool(r, "fullscreen"))
                    a.fullscreen = f;
            }
        }
    if (pinned.is_array())
        for (size_t i = 0; i < pinned.size(); ++i)
            if (pinned[i].is_string())
                get(pinned[i].get<std::string>()).dock = int(i);
    if (placements.is_object())
        for (const auto& [id, v] : placements.items()) {
            if (!v.is_object() || v.value("width", 0) <= 0 || v.value("height", 0) <= 0)
                continue;
            get(id).placement = Placement{v.value("output", ""), v.value("x", 0), v.value("y", 0),
                                          v.value("width", 0), v.value("height", 0), v.value("maximized", false),
                                          v.value("snapped", 0u)};
        }
    std::vector<AppRecord> out;
    for (auto& [_, a] : by_id)
        out.push_back(std::move(a));
    return out;
}

std::vector<ShortcutRecord> shortcuts_from_json(const json& binds) {
    std::vector<ShortcutRecord> out;
    if (!binds.is_array())
        return out;
    for (const json& b : binds) {
        if (!b.is_object() || !b.contains("keys") || !b.contains("action"))
            continue;
        out.push_back({0, b.value("keys", ""), b.value("action", ""), b.value("arg", ""), b.value("locked", false)});
    }
    return out;
}

json shortcut_json(const ShortcutRecord& s) {
    json j = {{"id", s.id}, {"keys", s.keys}, {"action", s.action}};
    if (!s.arg.empty())
        j["arg"] = s.arg;
    if (s.locked)
        j["locked"] = true;
    return j;
}

json app_json(const AppRecord& a) {
    json j = {{"app_id", a.app_id}, {"secret", a.secret}, {"space", a.space}, {"launch", a.launch}};
    j["dock"] = a.dock ? json(*a.dock) : json(nullptr);
    j["maximized"] = a.maximized ? json(*a.maximized) : json(nullptr);
    j["fullscreen"] = a.fullscreen ? json(*a.fullscreen) : json(nullptr);
    if (a.placement)
        j["placement"] = {{"output", a.placement->output}, {"x", a.placement->x}, {"y", a.placement->y},
                          {"width", a.placement->width}, {"height", a.placement->height},
                          {"maximized", a.placement->maximized}, {"snapped", a.placement->snapped}};
    return j;
}

json rule_json(const RuleRecord& r) {
    json j = {{"id", r.id}, {"app_pattern", r.app_pattern}, {"title_pattern", r.title_pattern},
              {"secret", r.secret}, {"space", r.space}, {"launch", r.launch}};
    j["maximized"] = r.maximized ? json(*r.maximized) : json(nullptr);
    j["fullscreen"] = r.fullscreen ? json(*r.fullscreen) : json(nullptr);
    return j;
}

} // namespace atrium
