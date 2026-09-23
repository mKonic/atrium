#pragma once
#include "config.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

using json = nlohmann::json;

enum class SettingType { Bool, Int, Float, String, Color, Choice, Keybinds, Rules, StringList };

// One setting as the Settings app sees it: enough to draw a control for it
// and validate what comes back.
struct SettingSchema {
    std::string key;          // "appearance.corner_radius"
    SettingType type;
    std::string title;        // "Corner radius"
    std::string description;
    std::string page;         // Settings app page: "Appearance"
    double min = 0, max = 0;  // Int / Float
    std::vector<std::string> choices;  // Choice
    json default_value;

    // Copies a validated value into the in-memory config.
    std::function<void(Config&, const json&)> apply;
};

// The settings store. atrium owns it: the Settings app and the CLI change it
// through the IPC socket, never by editing the file. Only values that differ
// from the defaults are written.
class Settings {
public:
    // `defaults` decides the default of every setting (it differs when
    // running nested).
    Settings(const Config& defaults, std::filesystem::path file);

    const std::vector<SettingSchema>& schema() const { return schema_; }
    const SettingSchema* find(const std::string& key) const;

    json get(const std::string& key) const;  // current value, default if unset
    json all() const;                        // every key → current value

    // Validate and store. Returns an error message instead when the key is
    // unknown or the value does not fit the schema.
    std::optional<std::string> set(const std::string& key, const json& value);
    std::optional<std::string> reset(const std::string& key);

    // Fill `config` from the current values.
    void apply(Config& config) const;

    bool load();  // false when the file exists but could not be read
    bool save() const;

    const std::filesystem::path& file() const { return file_; }

    // Default location: $XDG_CONFIG_HOME/atrium/settings.json.
    static std::filesystem::path default_file();

private:
    std::optional<std::string> validate(const SettingSchema& s, json& value) const;

    std::vector<SettingSchema> schema_;
    std::map<std::string, json> values_;  // only non-default values
    std::filesystem::path file_;
};

// --- text forms shared by the store, the IPC and the CLI ----------------------------

// "#rrggbb" or "#rrggbbaa" ↔ Color.
std::optional<Color> parse_color(const std::string& text);
std::string format_color(const Color& c);

// "Mod+Shift+E" → modifiers + keysym. "Mod" is the configured modifier.
struct KeyChord {
    uint32_t mods = 0;
    bool uses_mod = false;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
};
std::optional<KeyChord> parse_chord(const std::string& text);

// Action names as they appear in keybind settings and IPC ("close", "terminal").
std::optional<Action> action_from_name(const std::string& name);
const char* action_name(Action action);

// Default keybinds as settings JSON: [{"keys": "Mod+Q", "action": "close"}, ...].
json default_keybinds();

// Resolve keybind settings JSON against the configured modifier. Entries that
// fail to parse are skipped with the reason appended to `errors`.
std::vector<Keybind> resolve_keybinds(const json& binds, uint32_t mod,
                                      std::vector<std::string>* errors = nullptr);

uint32_t modifier_from_name(const std::string& name);  // "super", "alt", "ctrl"
const char* modifier_name(uint32_t mod);

} // namespace atrium
