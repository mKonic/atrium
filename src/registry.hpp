#pragma once
// The registry: everything atrium is told to remember, in one SQLite
// database only atrium writes ($XDG_CONFIG_HOME/atrium/registry.db). The
// Settings app and atriumctl change it through the IPC socket.
//
//   settings   typed values by key ("appearance.corner_radius" → 12); only
//              those that differ from the default are stored
//   apps       one record per app: the space its windows open in, whether
//              showing that space starts it, its place in the Dock, how its
//              windows open, where its window last was
//   rules      pattern rules for what an app record can't say (by title,
//              or a pattern over app ids)
//   shortcuts  key combinations and the actions they run
//   displays   how each monitor was last set up, by make, model and serial
//   devices    a mouse's or touchpad's own settings over the Mouse & Touchpad
//              ones, by device name
//   sessions   apps that ask for their windows back (xdg-session-management):
//              where each named window was, per session

#include "placements.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace atrium {

using json = nlohmann::json;

// An app, by the id its windows carry (Wayland app_id / X11 class, matched
// without regard to case).
struct AppRecord {
    std::string app_id;
    std::string secret;     // opens in this secret space ("" none)
    int space = 0;          // or this numbered space (0: wherever it would)
    std::string launch;     // with a secret space: command started when the space is shown
    std::optional<int> dock;  // position among the Dock's pins; none: not pinned
    std::optional<bool> maximized, fullscreen;
    std::optional<Placement> placement;  // remembered, as windows close

    bool operator==(const AppRecord&) const = default;
    // Nothing left worth keeping.
    bool empty() const;
};

// A pattern rule, for what an app record can't say.
struct RuleRecord {
    int64_t id = 0;
    std::string app_pattern, title_pattern;  // case-insensitive regular expressions
    std::string secret;
    int space = 0;
    std::string launch;
    std::optional<bool> maximized, fullscreen;

    bool operator==(const RuleRecord&) const = default;
};

struct ShortcutRecord {
    int64_t id = 0;
    std::string keys;    // "Mod+Shift+E"
    std::string action;  // "close"
    std::string arg;
    bool locked = false;  // also works on the lock screen

    bool operator==(const ShortcutRecord&) const = default;
};

// A monitor's setup, restored whenever it is plugged in.
struct DisplayRecord {
    std::string id;  // "make model serial" (or the connector name without them)
    bool enabled = true;
    int width = 0, height = 0, refresh = 0;  // refresh in mHz; 0 × 0: the preferred mode
    double scale = 1.0;
    int transform = 0;  // wl_output_transform
    std::optional<int> x, y;  // none: placed automatically
    std::string adaptive_sync = "games";  // variable refresh: off, games (fullscreen games only), on
    bool hdr = false;                     // HDR10 output, when the screen takes it
    int sdr_brightness = 30;              // 0-100 as Windows' "SDR content brightness": 80-480 nits

    bool operator==(const DisplayRecord&) const = default;
};

// One pointing device's own settings; what isn't set follows the shared ones.
struct DeviceRecord {
    std::string name;  // as libinput names it ("Logitech G502 HERO Gaming Mouse")
    std::optional<double> speed;              // -1 .. 1
    std::optional<std::string> acceleration;  // "adaptive" or "flat"
    std::optional<bool> natural_scroll, left_handed;

    bool operator==(const DeviceRecord&) const = default;
};

// One window of an app's session, by the name the app gave it.
struct SessionWindow {
    std::string name;
    Placement placement;
    bool fullscreen = false;

    bool operator==(const SessionWindow&) const = default;
};

class Registry {
public:
    // Opens (creating) the database. ":memory:" for a throwaway one.
    explicit Registry(const std::string& path);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    bool ok() const { return db_ != nullptr; }
    // Created by this open: the caller seeds defaults and imports old files.
    bool fresh() const { return fresh_; }

    // --- settings ---
    std::map<std::string, json> settings() const;
    void set_setting(const std::string& key, const json& value);
    void erase_setting(const std::string& key);

    // --- apps ---
    std::vector<AppRecord> apps() const;
    std::optional<AppRecord> app(const std::string& app_id) const;
    void put_app(const AppRecord& app);  // removes it when empty()
    void remove_app(const std::string& app_id);
    // The Dock's pins in order: renumbers `dock` on every app.
    void set_dock(const std::vector<std::string>& app_ids);

    // --- rules ---
    std::vector<RuleRecord> rules() const;
    int64_t add_rule(const RuleRecord& rule);
    bool update_rule(const RuleRecord& rule);
    bool remove_rule(int64_t id);

    // --- shortcuts ---
    std::vector<ShortcutRecord> shortcuts() const;
    int64_t add_shortcut(const ShortcutRecord& shortcut);
    bool update_shortcut(const ShortcutRecord& shortcut);
    bool remove_shortcut(int64_t id);
    void replace_shortcuts(const std::vector<ShortcutRecord>& shortcuts);

    // --- displays ---
    std::optional<DisplayRecord> display(const std::string& id) const;
    void put_display(const DisplayRecord& display);

    // --- devices ---
    std::optional<DeviceRecord> device(const std::string& name) const;
    std::vector<DeviceRecord> devices() const;
    // A record with nothing set is removed: the device follows the shared settings.
    void put_device(const DeviceRecord& device);

    // --- sessions ---
    bool has_session(const std::string& id) const;
    // Creates it, or marks it used now (unused sessions are dropped after a while).
    void touch_session(const std::string& id, const std::string& app_id);
    void remove_session(const std::string& id);
    std::optional<SessionWindow> session_window(const std::string& session, const std::string& name) const;
    void put_session_window(const std::string& session, const SessionWindow& window);
    void remove_session_window(const std::string& session, const std::string& name);
    void rename_session_window(const std::string& session, const std::string& from, const std::string& to);
    // Sessions not used since `before` (unix seconds) go, with their windows.
    void drop_sessions_before(int64_t before);

    // One transaction around many changes (an import).
    void begin();
    void commit();

    // Defaults path: $XDG_CONFIG_HOME/atrium/registry.db.
    static std::filesystem::path default_file();

private:
    void migrate();
    bool exec(const char* sql) const;

    std::string file_;  // "" for an in-memory one
    sqlite3* db_ = nullptr;
    bool fresh_ = false;
};

// Records from the JSON the old stores used (settings.json's windows.rules,
// dock.pinned and shortcuts.bindings; placements.json).
std::vector<AppRecord> apps_from_legacy(const json& rules, const json& pinned, const json& placements,
                                        std::vector<RuleRecord>* leftover_rules);
std::vector<ShortcutRecord> shortcuts_from_json(const json& binds);
json shortcut_json(const ShortcutRecord& s);
json app_json(const AppRecord& a);
json rule_json(const RuleRecord& r);

} // namespace atrium
