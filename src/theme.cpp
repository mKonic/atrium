#include "theme.hpp"

#include "accent.hpp"
#include "palette.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

#include <gio/gio.h>

extern "C" {
#include <wlr/util/log.h>
}

namespace atrium {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr char kWindowControls[] = {
#embed "../data/gtk/windowcontrols.css"
    , 0};
constexpr char kGtk4[] = {
#embed "../data/gtk/gtk-4.0.css"
    , 0};
constexpr char kGtk3[] = {
#embed "../data/gtk/gtk-3.0.css"
    , 0};
constexpr char kCloseSvg[] = {
#embed "../data/gtk/assets/close.svg"
    , 0};
constexpr char kMinimizeSvg[] = {
#embed "../data/gtk/assets/minimize.svg"
    , 0};
constexpr char kMaximizeSvg[] = {
#embed "../data/gtk/assets/maximize.svg"
    , 0};
constexpr char kIndex[] =
    "[Desktop Entry]\nType=X-GNOME-Metatheme\nName=atrium\n\n"
    "[X-GNOME-Metatheme]\nGtkTheme=atrium\n";

bool write_if_changed(const fs::path& path, std::string_view content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (std::ifstream in(path, std::ios::binary); in) {
        const std::string current((std::istreambuf_iterator<char>(in)), {});
        if (current == content)
            return true;
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out)
            return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

} // namespace

namespace {

// The accent over the stylesheet: a fill white text reads on, and a shade
// for accent-coloured text on the window background.
std::string accent_css(std::string_view accent, bool light) {
    const auto rgb = accent::seed(accent);
    if (!rgb)
        return {};
    const std::string fill = palette::hex(accent::tone(*rgb, std::min(accent::lightness(*rgb), 58.0)));
    const std::string text = palette::hex(accent::tone(*rgb, light ? 42 : 75));
    return "\n/* appearance.accent */\n"
           "@define-color accent_bg_color " + fill + ";\n"
           "@define-color accent_fg_color #ffffff;\n"
           "@define-color accent_color " + text + ";\n";
}

fs::path data_home() {
    if (const char* d = std::getenv("XDG_DATA_HOME"); d && *d)
        return d;
    if (const char* h = std::getenv("HOME"))
        return fs::path(h) / ".local/share";
    return {};
}

fs::path config_home() {
    if (const char* d = std::getenv("XDG_CONFIG_HOME"); d && *d)
        return d;
    if (const char* h = std::getenv("HOME"))
        return fs::path(h) / ".config";
    return {};
}

bool has_platform_theme(const char* plugin) {
    for (const char* dir : {"/usr/lib/qt6/plugins", "/usr/lib64/qt6/plugins", "/usr/lib/x86_64-linux-gnu/qt6/plugins"})
        if (fs::exists(fs::path(dir) / "platformthemes" / plugin))
            return true;
    return false;
}

// atrium's defaults for other programs' settings: first in XDG_CONFIG_DIRS,
// so anything the user set in their own ~/.config still wins.
fs::path defaults_dir() {
    const fs::path base = data_home();
    return base.empty() ? fs::path{} : base / "atrium/xdg";
}

void use_defaults_dir() {
    const fs::path xdg = defaults_dir();
    const char* dirs = std::getenv("XDG_CONFIG_DIRS");
    const std::string list = dirs && *dirs ? dirs : "/etc/xdg";
    if (!xdg.empty() && !list.starts_with(xdg.string() + ":"))
        setenv("XDG_CONFIG_DIRS", (xdg.string() + ":" + list).c_str(), 1);
}

// Without qtengine: plasma-integration's platform theme reads kdeglobals
// through XDG_CONFIG_DIRS, below the user's own, so a generated one first in
// that list fills whatever the user's file doesn't set. Not live: apps pick
// it up when they start.
void install_kdeglobals(bool light, std::string_view accent, const Interface& ui) {
    const fs::path xdg = defaults_dir();
    std::string text = palette::kde_colors(light, accent);
    if (!ui.icon_theme.empty())
        text += "\n[Icons]\nTheme=" + ui.icon_theme + "\n";
    auto font = [&](const std::string& family) {
        return family + "," + std::to_string(ui.font_size) + ",-1,5,400,0,0,0,0,0";
    };
    if (!ui.font.empty() || !ui.mono.empty()) {
        text += "\n[General]\n";
        if (!ui.font.empty())
            text += "font=" + font(ui.font) + "\n";
        if (!ui.mono.empty())
            text += "fixed=" + font(ui.mono) + "\n";
    }
    if (xdg.empty() || !write_if_changed(xdg / "kdeglobals", text)) {
        wlr_log(WLR_ERROR, "theme: couldn't write %s; Qt apps keep their own colours", xdg.c_str());
        return;
    }
    use_defaults_dir();
    setenv("QT_QPA_PLATFORMTHEME", "kde", 1);
}

} // namespace

void install_gtk_theme(bool light, std::string_view accent) {
    const fs::path base = data_home();
    if (base.empty())
        return;
    const fs::path dir = base / "themes/atrium";

    bool ok = write_if_changed(dir / "index.theme", kIndex);
    for (const char* v : {"gtk-4.0", "gtk-3.0"}) {
        ok &= write_if_changed(dir / v / "windowcontrols.css", kWindowControls);
        ok &= write_if_changed(dir / v / "assets/close.svg", kCloseSvg);
        ok &= write_if_changed(dir / v / "assets/minimize.svg", kMinimizeSvg);
        ok &= write_if_changed(dir / v / "assets/maximize.svg", kMaximizeSvg);
        // The shipped CSS builds on adw-gtk3-dark; light mode on adw-gtk3.
        std::string css = std::string_view(v) == "gtk-4.0" ? kGtk4 : kGtk3;
        if (light)
            if (size_t at = css.find("adw-gtk3-dark"); at != std::string::npos)
                css.replace(at, 13, "adw-gtk3");
        css += accent_css(accent, light);
        ok &= write_if_changed(dir / v / "gtk.css", css);
    }
    if (!ok) {
        wlr_log(WLR_ERROR, "theme: couldn't write %s; GTK apps keep their own buttons", dir.c_str());
        return;
    }
    setenv("GTK_THEME", "atrium", 1);
}

void install_qt_theme(bool light, std::string_view accent, const Interface& ui) {
    const fs::path base = data_home();
    if (base.empty())
        return;
    const fs::path dir = base / "atrium/qt";
    const char* set = std::getenv("QT_QPA_PLATFORMTHEME");
    const std::string_view qpa = set ? set : "";
    if ((qpa == "kde" || (qpa.empty() && !has_platform_theme("libqt6engine-plugin.so"))) &&
        has_platform_theme("KDEPlasmaPlatformTheme6.so")) {
        install_kdeglobals(light, accent, ui);
        return;
    }
    if (!has_platform_theme("libqt6engine-plugin.so") || (!qpa.empty() && qpa != "qtengine")) {
        // Neither: Qt's own portal theme at least follows dark and light,
        // live (Fusion's palette, not atrium's colours).
        if (qpa.empty() && has_platform_theme("libqxdgdesktopportal.so"))
            setenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal", 1);
        return;  // otherwise another platform theme the user picked
    }
    // Named for what it holds, so a switch changes config.json too: that is
    // what qtengine watches to recolour running apps.
    const fs::path colors = dir / ("atrium-" + std::string(light ? "light-" : "dark-") + std::string(accent) + ".colors");

    // The user's own qtengine settings (style, icons, fonts), with atrium's colours.
    json config = json::object();
    if (std::ifstream in(config_home() / "qtengine/config.json"); in)
        config = json::parse(in, nullptr, false);
    if (!config.is_object())
        config = json::object();
    json& theme = config["theme"];
    if (!theme.is_object())
        theme = json::object();
    theme["colorScheme"] = colors.string();
    // What atrium's settings name wins; the rest stays the user's.
    if (!ui.icon_theme.empty())
        theme["iconTheme"] = ui.icon_theme;
    if (!ui.font.empty())
        theme["font"] = {{"family", ui.font}, {"size", ui.font_size}, {"weight", -1}};
    if (!ui.mono.empty())
        theme["fontFixed"] = {{"family", ui.mono}, {"size", ui.font_size}, {"weight", -1}};
    if (!theme.contains("iconTheme"))
        theme["iconTheme"] = light ? "breeze" : "breeze-dark";
    if (!theme.contains("style"))
        theme["style"] = "Fusion";

    // The colours first: qtengine reloads when its config changes.
    if (!write_if_changed(colors, palette::kde_colors(light, accent)) ||
        !write_if_changed(dir / "config.json", config.dump(2) + "\n")) {
        wlr_log(WLR_ERROR, "theme: couldn't write %s; Qt apps keep their own colours", dir.c_str());
        return;
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".colors" && e.path() != colors)
            fs::remove(e.path(), ec);
    setenv("QT_QPA_PLATFORMTHEME", "qtengine", 1);
    setenv("QTENGINE_CONFIG", (dir / "config.json").c_str(), 1);
}

void install_app_defaults() {
    const fs::path xdg = defaults_dir();
    if (xdg.empty())
        return;
    // fcitx5's candidate popup: dark with the system and in the accent
    // colour (both follow the settings portal, live).
    constexpr char kFcitx5[] =
        "Theme=default\n"
        "DarkTheme=default-dark\n"
        "UseDarkTheme=True\n"
        "UseAccentColor=True\n";
    if (!write_if_changed(xdg / "fcitx5/conf/classicui.conf", kFcitx5))
        wlr_log(WLR_ERROR, "theme: couldn't write %s", xdg.c_str());
    use_defaults_dir();
}

void apply_color_scheme(bool light) {
    constexpr const char* kSchema = "org.gnome.desktop.interface";
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, kSchema, true) : nullptr;
    if (!schema)
        return;
    const bool has = g_settings_schema_has_key(schema, "color-scheme");
    g_settings_schema_unref(schema);
    if (!has)
        return;
    GSettings* s = g_settings_new(kSchema);
    g_settings_set_string(s, "color-scheme", light ? "prefer-light" : "prefer-dark");
    g_settings_sync();
    g_object_unref(s);
}

void apply_interface(const Interface& ui) {
    constexpr const char* kSchema = "org.gnome.desktop.interface";
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, kSchema, true) : nullptr;
    if (!schema)
        return;
    GSettings* s = g_settings_new(kSchema);
    auto set = [&](const char* key, const std::string& value) {
        if (!value.empty() && g_settings_schema_has_key(schema, key))
            g_settings_set_string(s, key, value.c_str());
    };
    set("icon-theme", ui.icon_theme);
    set("font-name", ui.font.empty() ? "" : ui.font + " " + std::to_string(ui.font_size));
    set("monospace-font-name", ui.mono.empty() ? "" : ui.mono + " " + std::to_string(ui.font_size));
    set("cursor-theme", ui.cursor_theme);
    if (g_settings_schema_has_key(schema, "cursor-size"))
        g_settings_set_int(s, "cursor-size", ui.cursor_size);
    g_settings_sync();
    g_object_unref(s);
    g_settings_schema_unref(schema);
}

void apply_accent_color(std::string_view accent) {
    constexpr const char* kSchema = "org.gnome.desktop.interface";
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, kSchema, true) : nullptr;
    if (!schema)
        return;
    const bool has = g_settings_schema_has_key(schema, "accent-color");  // GNOME 47 and later
    g_settings_schema_unref(schema);
    if (!has)
        return;
    GSettings* s = g_settings_new(kSchema);
    if (accent::seed(accent))
        g_settings_set_string(s, "accent-color", std::string(accent::gnome_name(accent)).c_str());
    else
        g_settings_reset(s, "accent-color");
    g_settings_sync();
    g_object_unref(s);
}

void apply_gtk_button_layout() {
    constexpr const char* kSchema = "org.gnome.desktop.wm.preferences";
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, kSchema, true) : nullptr;
    if (!schema)
        return;  // no GNOME schemas installed: GTK falls back to its own default
    g_settings_schema_unref(schema);
    GSettings* settings = g_settings_new(kSchema);
    g_settings_set_string(settings, "button-layout", ":minimize,maximize,close");
    g_settings_sync();
    g_object_unref(settings);
}

} // namespace atrium
