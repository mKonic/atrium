#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

extern "C" {
#include <wlr/util/log.h>
}

namespace atrium {

namespace fs = std::filesystem;

// --- text forms ------------------------------------------------------------------

std::optional<Color> parse_color(const std::string& text) {
    if (text.size() != 7 && text.size() != 9)
        return std::nullopt;
    if (text[0] != '#')
        return std::nullopt;
    Color c{0, 0, 0, 1};
    for (size_t i = 0; i < (text.size() - 1) / 2; ++i) {
        const std::string byte = text.substr(1 + 2 * i, 2);
        if (!std::isxdigit(byte[0]) || !std::isxdigit(byte[1]))
            return std::nullopt;
        c[i] = float(std::stoi(byte, nullptr, 16)) / 255.0f;
    }
    return c;
}

std::string format_color(const Color& c) {
    char buf[10];
    auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
    return buf;
}

namespace {

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return s;
}

struct ActionName {
    Action action;
    const char* name;
};
constexpr ActionName kActions[] = {
    {Action::Spawn, "spawn"},
    {Action::SpawnTerminal, "terminal"},
    {Action::CloseWindow, "close"},
    {Action::ToggleFullscreen, "fullscreen"},
    {Action::ToggleMaximize, "maximize"},
    {Action::Minimize, "minimize"},
    {Action::FocusNext, "focus-next"},
    {Action::FocusPrev, "focus-prev"},
    {Action::SwitchVt, "switch-vt"},
    {Action::Quit, "quit"},
    {Action::Space, "space"},
    {Action::MoveToSpace, "move-to-space"},
    {Action::SpacePrev, "space-prev"},
    {Action::SpaceNext, "space-next"},
    {Action::ToggleSecret, "toggle-secret"},
    {Action::MoveToSecret, "move-to-secret"},
};

} // namespace

std::optional<KeyChord> parse_chord(const std::string& text) {
    KeyChord chord;
    std::stringstream in(text);
    std::string part;
    std::vector<std::string> parts;
    while (std::getline(in, part, '+'))
        parts.push_back(part);
    // "Mod++" binds the plus key; a lone trailing "+" is unfinished.
    if (text.ends_with("++") && !parts.empty() && parts.back().empty())
        parts.back() = "plus";
    else if (text.ends_with("+"))
        return std::nullopt;
    if (parts.empty() || parts.back().empty())
        return std::nullopt;

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string m = lower(parts[i]);
        if (m == "mod")
            chord.uses_mod = true;
        else if (uint32_t bit = modifier_from_name(m))
            chord.mods |= bit;
        else
            return std::nullopt;
    }

    xkb_keysym_t sym = xkb_keysym_from_name(parts.back().c_str(), XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(parts.back().c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol)
        return std::nullopt;
    // Bindings match the unshifted symbol: "Mod+Shift+E" means the e key.
    chord.sym = xkb_keysym_to_lower(sym);
    return chord;
}

std::optional<Action> action_from_name(const std::string& name) {
    for (const auto& a : kActions)
        if (name == a.name)
            return a.action;
    return std::nullopt;
}

const char* action_name(Action action) {
    for (const auto& a : kActions)
        if (a.action == action)
            return a.name;
    return "unknown";
}

uint32_t modifier_from_name(const std::string& name) {
    const std::string n = lower(name);
    if (n == "super" || n == "logo" || n == "win" || n == "mod4") return WLR_MODIFIER_LOGO;
    if (n == "alt" || n == "mod1") return WLR_MODIFIER_ALT;
    if (n == "ctrl" || n == "control") return WLR_MODIFIER_CTRL;
    if (n == "shift") return WLR_MODIFIER_SHIFT;
    return 0;
}

const char* modifier_name(uint32_t mod) {
    switch (mod) {
    case WLR_MODIFIER_LOGO: return "super";
    case WLR_MODIFIER_ALT: return "alt";
    case WLR_MODIFIER_CTRL: return "ctrl";
    case WLR_MODIFIER_SHIFT: return "shift";
    default: return "super";
    }
}

json default_keybinds() {
    json binds = json::array({
        {{"keys", "Mod+Return"}, {"action", "terminal"}},
        {{"keys", "Mod+Q"}, {"action", "close"}},
        {{"keys", "Mod+F"}, {"action", "fullscreen"}},
        {{"keys", "Mod+Up"}, {"action", "maximize"}},
        {{"keys", "Mod+H"}, {"action", "minimize"}},
        {{"keys", "Mod+Tab"}, {"action", "focus-next"}},
        {{"keys", "Mod+Shift+Tab"}, {"action", "focus-prev"}},
        {{"keys", "Mod+Shift+E"}, {"action", "quit"}},
        {{"keys", "Mod+Ctrl+Left"}, {"action", "space-prev"}},
        {{"keys", "Mod+Ctrl+Right"}, {"action", "space-next"}},
        {{"keys", "Mod+D"}, {"action", "toggle-secret"}, {"arg", "communication"}},
        {{"keys", "Mod+Shift+D"}, {"action", "move-to-secret"}, {"arg", "communication"}},
    });
    for (int n = 1; n <= 9; ++n) {
        binds.push_back({{"keys", "Mod+" + std::to_string(n)}, {"action", "space"}, {"arg", std::to_string(n)}});
        binds.push_back({{"keys", "Mod+Shift+" + std::to_string(n)}, {"action", "move-to-space"},
                         {"arg", std::to_string(n)}});
    }
    for (int vt = 1; vt <= 12; ++vt)
        binds.push_back({{"keys", "Ctrl+Alt+F" + std::to_string(vt)},
                         {"action", "switch-vt"},
                         {"arg", std::to_string(vt)}});
    return binds;
}

std::vector<Keybind> resolve_keybinds(const json& binds, uint32_t mod, std::vector<std::string>* errors) {
    std::vector<Keybind> out;
    auto fail = [&](const std::string& why) {
        if (errors)
            errors->push_back(why);
    };
    if (!binds.is_array()) {
        fail("keybinds must be a list");
        return out;
    }
    for (const json& b : binds) {
        if (!b.is_object() || !b.contains("keys") || !b["keys"].is_string() || !b.contains("action") ||
            !b["action"].is_string()) {
            fail("each keybind needs \"keys\" and \"action\"");
            continue;
        }
        const std::string keys = b["keys"];
        auto chord = parse_chord(keys);
        if (!chord) {
            fail("can't read key combination '" + keys + "'");
            continue;
        }
        auto action = action_from_name(b["action"]);
        if (!action) {
            fail("unknown action '" + b["action"].get<std::string>() + "'");
            continue;
        }
        Keybind k{chord->mods | (chord->uses_mod ? mod : 0), chord->sym, *action};
        if (b.contains("arg") && b["arg"].is_string())
            k.arg = b["arg"];
        if (*action == Action::SwitchVt || *action == Action::Space || *action == Action::MoveToSpace)
            k.iarg = std::atoi(k.arg.c_str());
        if ((*action == Action::Spawn || *action == Action::ToggleSecret || *action == Action::MoveToSecret) &&
            k.arg.empty()) {
            fail("'" + keys + "': " + b["action"].get<std::string>() + " needs \"arg\"");
            continue;
        }
        if ((*action == Action::Space || *action == Action::MoveToSpace) && (k.iarg < 1 || k.iarg > 99)) {
            fail("'" + keys + "': space numbers go from 1 to 99");
            continue;
        }
        out.push_back(std::move(k));
    }
    return out;
}

// --- schema ------------------------------------------------------------------------

namespace {

SettingSchema make(std::string key, SettingType type, std::string page, std::string title,
                   std::string description, json def, std::function<void(Config&, const json&)> apply) {
    SettingSchema s;
    s.key = std::move(key);
    s.type = type;
    s.page = std::move(page);
    s.title = std::move(title);
    s.description = std::move(description);
    s.default_value = std::move(def);
    s.apply = std::move(apply);
    return s;
}

template <typename T>
SettingSchema number(std::string key, SettingType type, std::string page, std::string title,
                     std::string desc, T Config::*field, const Config& d, double min, double max) {
    auto s = make(std::move(key), type, std::move(page), std::move(title), std::move(desc), d.*field,
                  [field](Config& c, const json& v) { c.*field = v.get<T>(); });
    s.min = min;
    s.max = max;
    return s;
}

SettingSchema boolean(std::string key, std::string page, std::string title, std::string desc,
                      bool Config::*field, const Config& d) {
    return make(std::move(key), SettingType::Bool, std::move(page), std::move(title), std::move(desc),
                d.*field, [field](Config& c, const json& v) { c.*field = v.get<bool>(); });
}

SettingSchema text(std::string key, std::string page, std::string title, std::string desc,
                   std::string Config::*field, const Config& d) {
    return make(std::move(key), SettingType::String, std::move(page), std::move(title), std::move(desc),
                d.*field, [field](Config& c, const json& v) { c.*field = v.get<std::string>(); });
}

SettingSchema color(std::string key, std::string page, std::string title, std::string desc,
                    Color Config::*field, const Config& d) {
    return make(std::move(key), SettingType::Color, std::move(page), std::move(title), std::move(desc),
                format_color(d.*field), [field](Config& c, const json& v) {
                    if (auto col = parse_color(v.get<std::string>()))
                        c.*field = *col;
                });
}

SettingSchema choice(std::string key, std::string page, std::string title, std::string desc,
                     std::vector<std::string> choices, json def,
                     std::function<void(Config&, const json&)> apply) {
    auto s = make(std::move(key), SettingType::Choice, std::move(page), std::move(title), std::move(desc),
                  std::move(def), std::move(apply));
    s.choices = std::move(choices);
    return s;
}

std::vector<SettingSchema> build_schema(const Config& d) {
    using T = SettingType;
    std::vector<SettingSchema> s;

    // Appearance
    s.push_back(number("appearance.corner_radius", T::Int, "Appearance", "Corner radius",
        "Roundness of window corners, in pixels.", &Config::corner_radius, d, 0, 64));
    s.push_back(boolean("appearance.shadows", "Appearance", "Window shadows",
        "Draw a soft shadow under windows.", &Config::shadows, d));
    s.push_back(number("appearance.shadow_size", T::Float, "Appearance", "Shadow size",
        "Blur radius of the focused window's shadow.", &Config::shadow_sigma, d, 0, 100));
    s.push_back(number("appearance.shadow_size_inactive", T::Float, "Appearance", "Inactive shadow size",
        "Blur radius of other windows' shadows.", &Config::shadow_sigma_inactive, d, 0, 100));
    s.push_back(color("appearance.shadow_color", "Appearance", "Shadow color",
        "Color of the focused window's shadow.", &Config::shadow_color, d));
    s.push_back(color("appearance.shadow_color_inactive", "Appearance", "Inactive shadow color",
        "Color of other windows' shadows.", &Config::shadow_color_inactive, d));
    s.push_back(color("appearance.outline_color", "Appearance", "Window outline",
        "Hairline around the focused window; fully transparent turns it off.", &Config::outline_color, d));
    s.push_back(color("appearance.outline_color_inactive", "Appearance", "Inactive window outline",
        "Hairline around other windows.", &Config::outline_color_inactive, d));
    s.push_back(color("appearance.background", "Appearance", "Desktop color",
        "Shown where no wallpaper covers the screen.", &Config::background, d));

    // Motion
    s.push_back(boolean("appearance.animations", "Appearance", "Animations",
        "Windows and spaces move instead of jumping.", &Config::animations, d));
    s.push_back(number("appearance.animation_speed", T::Float, "Appearance", "Animation speed",
        "1 is normal; 2 is twice as fast.", &Config::animation_speed, d, 0.25, 4));

    // Windows
    s.push_back(number("windows.snap_distance", T::Int, "Windows", "Edge snapping",
        "Distance from a screen edge at which a dragged window sticks to it.", &Config::snap_distance, d, 0, 200));
    s.push_back(make("windows.rules", SettingType::Rules, "Windows", "Window rules",
        "Where windows of an app open and how, matched by app id or title.", default_rules(),
        [](Config& c, const json& v) { c.rules = parse_rules(v); }));
    s.push_back(color("windows.secret_backdrop", "Windows", "Secret space backdrop",
        "Color that dims the screen behind a secret space.", &Config::secret_backdrop, d));
    s.push_back(number("windows.cascade_step", T::Int, "Windows", "Cascade offset",
        "How far a new window steps down and right when it would cover another.", &Config::cascade_step, d, 0, 200));

    // Keyboard
    s.push_back(text("keyboard.layout", "Keyboard", "Layout",
        "Keyboard layouts, comma-separated (us, de, ...).", &Config::xkb_layout, d));
    s.push_back(text("keyboard.variant", "Keyboard", "Variant", "Layout variants, comma-separated.",
        &Config::xkb_variant, d));
    s.push_back(text("keyboard.options", "Keyboard", "Options",
        "XKB options, e.g. caps:escape.", &Config::xkb_options, d));
    s.push_back(text("keyboard.model", "Keyboard", "Model", "Keyboard model.", &Config::xkb_model, d));
    s.push_back(number("keyboard.repeat_rate", T::Int, "Keyboard", "Key repeat rate",
        "Repeats per second while a key is held.", &Config::repeat_rate, d, 1, 100));
    s.push_back(number("keyboard.repeat_delay", T::Int, "Keyboard", "Delay until repeat",
        "Milliseconds a key is held before it repeats.", &Config::repeat_delay, d, 100, 2000));

    // Mouse and touchpad
    s.push_back(number("pointer.speed", T::Float, "Mouse & Touchpad", "Pointer speed",
        "From -1 (slowest) to 1 (fastest).", &Config::accel_speed, d, -1, 1));
    s.push_back(choice("pointer.acceleration", "Mouse & Touchpad", "Acceleration",
        "Adaptive speeds up fast movements; flat keeps the pointer linear.", {"adaptive", "flat"},
        d.accel_profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT ? "flat" : "adaptive",
        [](Config& c, const json& v) {
            c.accel_profile = v == "flat" ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                                          : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
        }));
    s.push_back(boolean("pointer.natural_scroll", "Mouse & Touchpad", "Natural scrolling (mouse)",
        "Content follows the wheel, like a touchscreen.", &Config::natural_scroll, d));
    s.push_back(boolean("pointer.left_handed", "Mouse & Touchpad", "Left-handed",
        "Swap the primary and secondary buttons.", &Config::left_handed, d));
    s.push_back(boolean("pointer.middle_emulation", "Mouse & Touchpad", "Middle-click emulation",
        "Pressing left and right together is a middle click.", &Config::middle_button_emulation, d));
    s.push_back(boolean("touchpad.natural_scroll", "Mouse & Touchpad", "Natural scrolling (touchpad)",
        "Content follows your fingers.", &Config::touchpad_natural_scroll, d));
    s.push_back(boolean("touchpad.tap_to_click", "Mouse & Touchpad", "Tap to click",
        "A tap on the touchpad is a click.", &Config::tap_to_click, d));
    s.push_back(boolean("touchpad.tap_and_drag", "Mouse & Touchpad", "Tap and drag",
        "Tap, then touch again and move, to drag.", &Config::tap_and_drag, d));
    s.push_back(boolean("touchpad.drag_lock", "Mouse & Touchpad", "Drag lock",
        "A tap-drag continues after lifting the finger briefly.", &Config::drag_lock, d));
    s.push_back(boolean("touchpad.disable_while_typing", "Mouse & Touchpad", "Ignore while typing",
        "The touchpad pauses while you type.", &Config::disable_while_typing, d));

    // Cursor
    s.push_back(text("cursor.theme", "Appearance", "Cursor theme",
        "Name of an installed cursor theme; empty for the default.", &Config::cursor_theme, d));
    s.push_back(number("cursor.size", T::Int, "Appearance", "Cursor size", "Cursor size in pixels.",
        &Config::cursor_size, d, 8, 256));

    // Power
    s.push_back(boolean("power.hidden_windows_keep_awake", "Power", "Hidden windows keep the screen on",
        "A video playing in a window you can't see still stops the screen from sleeping.",
        &Config::idle_inhibit_ignore_visibility, d));

    // Shortcuts (the modifier must apply before the bindings that use it)
    s.push_back(choice("shortcuts.modifier", "Keyboard Shortcuts", "Shortcut key",
        "The key written as Mod in shortcuts.", {"super", "alt", "ctrl"}, modifier_name(d.mod),
        [](Config& c, const json& v) { c.mod = modifier_from_name(v.get<std::string>()); }));
    s.push_back(text("shortcuts.terminal", "Keyboard Shortcuts", "Terminal",
        "Command the terminal shortcut runs.", &Config::terminal, d));
    s.push_back(make("shortcuts.bindings", SettingType::Keybinds, "Keyboard Shortcuts", "Shortcuts",
        "Key combinations and what they do.", default_keybinds(),
        [](Config& c, const json& v) { c.keybinds = resolve_keybinds(v, c.mod); }));

    return s;
}

} // namespace

// --- store ---------------------------------------------------------------------------

Settings::Settings(const Config& defaults, fs::path file) : schema_(build_schema(defaults)), file_(std::move(file)) {}

fs::path Settings::default_file() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "atrium" / "settings.json";
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".config" / "atrium" / "settings.json";
    return "settings.json";
}

const SettingSchema* Settings::find(const std::string& key) const {
    for (const auto& s : schema_)
        if (s.key == key)
            return &s;
    return nullptr;
}

json Settings::get(const std::string& key) const {
    if (auto it = values_.find(key); it != values_.end())
        return it->second;
    if (const auto* s = find(key))
        return s->default_value;
    return nullptr;
}

json Settings::all() const {
    json out = json::object();
    for (const auto& s : schema_)
        out[s.key] = get(s.key);
    return out;
}

std::optional<std::string> Settings::validate(const SettingSchema& s, json& v) const {
    switch (s.type) {
    case SettingType::Bool:
        if (!v.is_boolean())
            return s.key + " is on/off (true or false)";
        break;
    case SettingType::Int: {
        if (v.is_number_float() && std::floor(v.get<double>()) == v.get<double>())
            v = v.get<int64_t>();
        if (!v.is_number_integer())
            return s.key + " is a whole number";
        const auto n = v.get<int64_t>();
        if (n < s.min || n > s.max)
            return s.key + " goes from " + std::to_string(int64_t(s.min)) + " to " +
                   std::to_string(int64_t(s.max));
        break;
    }
    case SettingType::Float: {
        if (!v.is_number())
            return s.key + " is a number";
        const double n = v.get<double>();
        if (n < s.min || n > s.max) {
            std::ostringstream msg;
            msg << s.key << " goes from " << s.min << " to " << s.max;
            return msg.str();
        }
        v = n;
        break;
    }
    case SettingType::String:
        if (!v.is_string())
            return s.key + " is text";
        break;
    case SettingType::Color:
        if (!v.is_string() || !parse_color(v.get<std::string>()))
            return s.key + " is a color like #1a1b26 or #1a1b26cc";
        v = format_color(*parse_color(v.get<std::string>()));
        break;
    case SettingType::Choice:
        if (!v.is_string() || std::ranges::find(s.choices, v.get<std::string>()) == s.choices.end()) {
            std::string all;
            for (const auto& c : s.choices)
                all += (all.empty() ? "" : ", ") + c;
            return s.key + " is one of: " + all;
        }
        break;
    case SettingType::Keybinds: {
        std::vector<std::string> errors;
        resolve_keybinds(v, WLR_MODIFIER_LOGO, &errors);
        if (!errors.empty())
            return errors.front();
        break;
    }
    case SettingType::Rules: {
        std::vector<std::string> errors;
        parse_rules(v, &errors);
        if (!errors.empty())
            return errors.front();
        break;
    }
    }
    return std::nullopt;
}

std::optional<std::string> Settings::set(const std::string& key, const json& value) {
    const SettingSchema* s = find(key);
    if (!s)
        return "no setting called " + key;
    json v = value;
    if (auto err = validate(*s, v))
        return err;
    if (v == s->default_value)
        values_.erase(key);
    else
        values_[key] = std::move(v);
    return std::nullopt;
}

std::optional<std::string> Settings::reset(const std::string& key) {
    if (!find(key))
        return "no setting called " + key;
    values_.erase(key);
    return std::nullopt;
}

void Settings::apply(Config& config) const {
    for (const auto& s : schema_)
        s.apply(config, get(s.key));
}

bool Settings::load() {
    std::ifstream in(file_);
    if (!in)
        return !fs::exists(file_);
    json doc;
    try {
        in >> doc;
    } catch (const json::exception& e) {
        wlr_log(WLR_ERROR, "settings: %s is unreadable (%s); using defaults", file_.c_str(), e.what());
        return false;
    }
    if (!doc.is_object())
        return false;
    // Keys that no longer exist or no longer validate are dropped, not fatal:
    // an old file must never keep the desktop from starting.
    for (auto& [key, value] : doc.items())
        if (auto err = set(key, value))
            wlr_log(WLR_INFO, "settings: ignoring %s: %s", key.c_str(), err->c_str());
    return true;
}

bool Settings::save() const {
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    json doc = json::object();
    for (const auto& [k, v] : values_)
        doc[k] = v;
    // Write-then-rename: a crash mid-write leaves the old file intact.
    const fs::path tmp = file_.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << doc.dump(2) << '\n';
        if (!out)
            return false;
    }
    fs::rename(tmp, file_, ec);
    return !ec;
}

} // namespace atrium
