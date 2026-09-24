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
    {Action::SnapLeft, "snap-left"},
    {Action::SnapRight, "snap-right"},
    {Action::Restore, "restore"},
    {Action::Overview, "overview"},
    {Action::AppExpose, "app-expose"},
    {Action::SwitchNext, "switch-next"},
    {Action::SwitchPrev, "switch-prev"},
    {Action::CycleSpaceNext, "cycle-space-next"},
    {Action::CycleSpacePrev, "cycle-space-prev"},
    {Action::RestartShell, "restart-shell"},
    {Action::Shell, "shell"},
    {Action::ToggleTiling, "toggle-tiling"},
    {Action::FocusDirection, "focus-direction"},
    {Action::MoveDirection, "move-direction"},
    {Action::MoveToSpacePrev, "move-to-space-prev"},
    {Action::MoveToSpaceNext, "move-to-space-next"},
    {Action::ToggleFloating, "toggle-floating"},
    {Action::TogglePin, "toggle-pin"},
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

std::map<int64_t, std::vector<int64_t>> shortcut_clashes(const std::vector<ShortcutRecord>& shortcuts, uint32_t mod) {
    std::map<std::pair<uint32_t, xkb_keysym_t>, std::vector<int64_t>> by_keys;
    for (const ShortcutRecord& s : shortcuts)
        if (auto chord = parse_chord(s.keys))
            by_keys[{chord->mods | (chord->uses_mod ? mod : 0), chord->sym}].push_back(s.id);
    std::map<int64_t, std::vector<int64_t>> out;
    for (const auto& [keys, ids] : by_keys)
        if (ids.size() > 1)
            for (int64_t id : ids)
                for (int64_t other : ids)
                    if (other != id)
                        out[id].push_back(other);
    return out;
}

std::optional<Action> action_from_name(const std::string& name) {
    for (const auto& a : kActions)
        if (name == a.name)
            return a.action;
    return std::nullopt;
}

std::vector<std::string> action_names() {
    std::vector<std::string> out;
    for (const auto& a : kActions)
        out.push_back(a.name);
    return out;
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
        {{"keys", "Mod+Alt+F"}, {"action", "maximize"}},
        {{"keys", "Mod+H"}, {"action", "minimize"}},
        {{"keys", "Alt+Tab"}, {"action", "switch-next"}},
        {{"keys", "Alt+Shift+Tab"}, {"action", "switch-prev"}},
        {{"keys", "Mod+Tab"}, {"action", "cycle-space-next"}},
        {{"keys", "Mod+Shift+Tab"}, {"action", "cycle-space-prev"}},
        {{"keys", "Mod+Shift+E"}, {"action", "quit"}},
        {{"keys", "Mod+Left"}, {"action", "focus-direction"}, {"arg", "left"}},
        {{"keys", "Mod+Right"}, {"action", "focus-direction"}, {"arg", "right"}},
        {{"keys", "Mod+Up"}, {"action", "focus-direction"}, {"arg", "up"}},
        {{"keys", "Mod+Down"}, {"action", "focus-direction"}, {"arg", "down"}},
        {{"keys", "Mod+Shift+Left"}, {"action", "move-direction"}, {"arg", "left"}},
        {{"keys", "Mod+Shift+Right"}, {"action", "move-direction"}, {"arg", "right"}},
        {{"keys", "Mod+Shift+Up"}, {"action", "move-direction"}, {"arg", "up"}},
        {{"keys", "Mod+Shift+Down"}, {"action", "move-direction"}, {"arg", "down"}},
        {{"keys", "Mod+Ctrl+Shift+Left"}, {"action", "move-to-space-prev"}},
        {{"keys", "Mod+Ctrl+Shift+Right"}, {"action", "move-to-space-next"}},
        {{"keys", "Mod+Alt+Space"}, {"action", "toggle-floating"}},
        {{"keys", "Mod+P"}, {"action", "toggle-pin"}},
        {{"keys", "Mod+Page_Up"}, {"action", "space-prev"}},
        {{"keys", "Mod+Page_Down"}, {"action", "space-next"}},
        {{"keys", "Mod+Ctrl+Left"}, {"action", "space-prev"}},
        {{"keys", "Mod+Ctrl+Right"}, {"action", "space-next"}},
        {{"keys", "Mod+Ctrl+Up"}, {"action", "overview"}},
        {{"keys", "Mod+Ctrl+Down"}, {"action", "app-expose"}},
        {{"keys", "Mod+Space"}, {"action", "shell"}, {"arg", "launcher"}},
        {{"keys", "Mod+V"}, {"action", "shell"}, {"arg", "clipboard"}},
        {{"keys", "Mod+comma"}, {"action", "shell"}, {"arg", "settings"}},
        {{"keys", "Mod+D"}, {"action", "toggle-secret"}, {"arg", "communication"}},
        {{"keys", "Mod+Shift+D"}, {"action", "move-to-secret"}, {"arg", "communication"}},
        {{"keys", "Mod+backslash"}, {"action", "toggle-tiling"}},
    });
    // Media keys go to the shell, which holds the audio and display
    // connections and shows what changed. Shift makes the steps finer.
    const std::pair<const char*, const char*> media[] = {
        {"XF86AudioRaiseVolume", "volume-up"}, {"Shift+XF86AudioRaiseVolume", "volume-up-fine"},
        {"XF86AudioLowerVolume", "volume-down"}, {"Shift+XF86AudioLowerVolume", "volume-down-fine"},
        {"XF86AudioMute", "volume-mute"}, {"XF86AudioMicMute", "mic-mute"},
        {"XF86MonBrightnessUp", "brightness-up"}, {"Shift+XF86MonBrightnessUp", "brightness-up-fine"},
        {"XF86MonBrightnessDown", "brightness-down"}, {"Shift+XF86MonBrightnessDown", "brightness-down-fine"},
        {"XF86AudioPlay", "media-play-pause"}, {"XF86AudioPause", "media-play-pause"},
        {"XF86AudioNext", "media-next"}, {"XF86AudioPrev", "media-previous"},
    };
    for (const auto& [keys, arg] : media)
        binds.push_back({{"keys", keys}, {"action", "shell"}, {"arg", arg}, {"locked", true}});
    for (int n = 1; n <= 10; ++n) {
        const std::string key = std::to_string(n % 10);  // Mod+0 is space 10
        binds.push_back({{"keys", "Mod+" + key}, {"action", "space"}, {"arg", std::to_string(n)}});
        binds.push_back({{"keys", "Mod+Shift+" + key}, {"action", "move-to-space"}, {"arg", std::to_string(n)}});
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
        if (b.contains("locked") && b["locked"].is_boolean())
            k.locked = b["locked"];
        if (*action == Action::SwitchVt || *action == Action::Space || *action == Action::MoveToSpace)
            k.iarg = std::atoi(k.arg.c_str());
        if ((*action == Action::Spawn || *action == Action::ToggleSecret || *action == Action::MoveToSecret ||
             *action == Action::FocusDirection || *action == Action::MoveDirection) &&
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
    s.push_back(choice("appearance.style", "Appearance", "Appearance",
        "Dark or light: the bar, panels, title bars and apps that follow the system.",
        {"dark", "light"}, d.light ? "light" : "dark",
        [](Config& c, const json& v) { c.light = v.get<std::string>() == "light"; }));
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

    s.push_back(boolean("appearance.transparency", "Appearance", "Transparent windows",
        "Let windows with translucent backgrounds show what is behind them. Off gives every window a solid background.",
        &Config::transparency, d));
    // Blur
    s.push_back(boolean("appearance.blur", "Appearance", "Blur",
        "Frosted glass behind translucent windows, panels and secret spaces.", &Config::blur, d));
    s.push_back(make("appearance.blurred_panels", SettingType::StringList, "Appearance", "Frosted panels",
        "Panels (by layer namespace; a trailing * matches the rest) that get blur behind them when transparency is on.",
        json(d.blurred_panels), [](Config& c, const json& v) { c.blurred_panels = v.get<std::vector<std::string>>(); }));
    s.push_back(number("appearance.blur_radius", T::Int, "Appearance", "Blur radius",
        "How far each blur pass reaches.", &Config::blur_radius, d, 1, 20));
    s.push_back(number("appearance.blur_passes", T::Int, "Appearance", "Blur strength",
        "More passes give a softer blur.", &Config::blur_passes, d, 1, 8));

    // Motion
    s.push_back(boolean("appearance.animations", "Appearance", "Animations",
        "Windows and spaces move instead of jumping.", &Config::animations, d));
    s.push_back(number("appearance.animation_speed", T::Float, "Appearance", "Animation speed",
        "1 is normal; 2 is twice as fast.", &Config::animation_speed, d, 0.25, 4));

    // Windows
    s.push_back(number("windows.snap_distance", T::Int, "Windows", "Edge snapping",
        "Distance from a screen edge at which a dragged window sticks to it.", &Config::snap_distance, d, 0, 200));
    s.push_back(color("windows.secret_backdrop", "Windows", "Secret space backdrop",
        "Color that dims the screen behind a secret space.", &Config::secret_backdrop, d));
    s.push_back(number("windows.secret_margin", T::Int, "Windows", "Secret space margin",
        "Windows in a secret space open large, with this much of the blurred desktop showing around them "
        "(percent of the screen).", &Config::secret_margin, d, 0, 25));
    s.push_back(boolean("windows.snapping", "Windows", "Snap to edges",
        "Drag a window to a screen edge or corner to fill half or a quarter of it; to the top to maximize.",
        &Config::snapping, d));
    s.push_back(number("windows.snap_gap", T::Int, "Windows", "Gap between snapped windows",
        "Space around and between windows snapped side by side.", &Config::snap_gap, d, 0, 64));
    s.push_back(boolean("windows.tiled_titlebars", "Windows", "Title bars on tiled windows",
        "Keep title bars on windows snapped to halves and quarters. Off gives the room to the window.",
        &Config::tiled_titlebars, d));
    s.push_back(boolean("windows.remember_placement", "Windows", "Reopen windows where they were",
        "An app's first window comes back at the size and place it had when it last closed.",
        &Config::remember_placement, d));
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

    // Session
    s.push_back(text("session.shell", "Session", "Desktop shell",
        "What draws the bar, Dock and the rest: \"builtin\" for atrium's own, a command for another, or \"none\".",
        &Config::shell, d));

    // Notifications (read by the shell)
    s.push_back(make("notifications.dnd", SettingType::Bool, "Notifications", "Do Not Disturb",
        "Keep notifications quiet: they go straight to the notification center without popping up.", false,
        [](Config&, const json&) {}));

    // Desktop (read by the shell)
    s.push_back(make("desktop.icons", SettingType::Bool, "Desktop", "Files on the desktop",
        "Show what is in the desktop folder as icons on the desktop.", true, [](Config&, const json&) {}));

    s.push_back(choice("power.profile", "Power", "Power mode",
        "How the system balances speed against power use. Applied at login and when changed.",
        {"performance", "balanced", "power-saver"}, d.power_profile,
        [](Config& c, const json& v) { c.power_profile = v.get<std::string>(); }));
    s.push_back(boolean("displays.allow_tearing", "Displays", "Allow tearing in games",
        "Fullscreen games that ask for it show each frame the moment it is ready instead of waiting for the "
        "screen's refresh: less input lag, at the cost of a visible tear line.", &Config::allow_tearing, d));
    s.push_back(number("displays.brightness", T::Int, "Displays", "Brightness",
        "Brightness of external monitors (DDC/CI). They forget it on boot, so atrium sets it again at login.",
        &Config::brightness, d, 0, 100));
    s.push_back(make("recording.audio", SettingType::Bool, "Screen Recording", "Record sound",
        "Include what the speakers play in screen recordings.", false, [](Config&, const json&) {}));
    s.push_back(boolean("session.clipboard_history", "Session", "Clipboard history",
        "Remember what you copy (text and pictures) so Super+V can bring it back.",
        &Config::clipboard_history, d));

    // Dock (read by the shell; the compositor itself has no use for them)
    s.push_back(make("dock.autohide", SettingType::Bool, "Dock", "Hide the Dock",
        "Keep the Dock out of sight until the pointer reaches the bottom of the screen.", false,
        [](Config&, const json&) {}));
    s.push_back(make("security.remember_admin", SettingType::Bool, "Privacy & Security", "Remember admin password",
        "After you enter your password for an administrator action, don't ask again for five minutes, as sudo does.",
        true, [](Config&, const json&) {}));
    s.push_back(make("bar.net_speed", SettingType::Bool, "Menu Bar", "Network speed",
        "Show download and upload speed next to the network icon.", true, [](Config&, const json&) {}));
    s.push_back(make("dock.magnify", SettingType::Bool, "Dock", "Magnification",
        "Icons grow as the pointer passes over them.", false, [](Config&, const json&) {}));

    // Shortcuts (the modifier must apply before the bindings that use it)
    s.push_back(choice("shortcuts.modifier", "Keyboard Shortcuts", "Shortcut key",
        "The key written as Mod in shortcuts.", {"super", "alt", "ctrl"}, modifier_name(d.mod),
        [](Config& c, const json& v) { c.mod = modifier_from_name(v.get<std::string>()); }));
    s.push_back(text("shortcuts.terminal", "Keyboard Shortcuts", "Terminal",
        "Command the terminal shortcut runs.", &Config::terminal, d));

    return s;
}

} // namespace

// --- store ---------------------------------------------------------------------------

json default_dock() {
    return json::array({"org.kde.dolphin", "com.mitchellh.ghostty", "google-chrome", "code-oss", "obsidian", "discord",
                        "spotify", "steam"});
}

Settings::Settings(const Config& defaults, Registry* registry)
    : schema_(build_schema(defaults)), registry_(registry) {}

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
    case SettingType::StringList:
        if (!v.is_array() || !std::ranges::all_of(v, [](const json& e) { return e.is_string(); }))
            return s.key + " is a list of text";
        break;
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
    if (v == s->default_value) {
        values_.erase(key);
        if (registry_)
            registry_->erase_setting(key);
    } else {
        if (registry_)
            registry_->set_setting(key, v);
        values_[key] = std::move(v);
    }
    return std::nullopt;
}

std::optional<std::string> Settings::reset(const std::string& key) {
    if (!find(key))
        return "no setting called " + key;
    values_.erase(key);
    if (registry_)
        registry_->erase_setting(key);
    return std::nullopt;
}

void Settings::apply(Config& config) const {
    for (const auto& s : schema_)
        s.apply(config, get(s.key));
}

void Settings::load() {
    if (!registry_)
        return;
    // Keys that no longer exist or no longer validate are dropped, not fatal:
    // an old value must never keep the desktop from starting.
    for (const auto& [key, value] : registry_->settings())
        if (auto err = set(key, value)) {
            wlr_log(WLR_INFO, "settings: dropping %s: %s", key.c_str(), err->c_str());
            registry_->erase_setting(key);
        }
}

void Settings::import(const json& doc) {
    if (!doc.is_object())
        return;
    for (const auto& [key, value] : doc.items())
        if (find(key))
            if (auto err = set(key, value))
                wlr_log(WLR_INFO, "settings: not importing %s: %s", key.c_str(), err->c_str());
}

} // namespace atrium
