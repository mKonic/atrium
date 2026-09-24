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

bool has_qtengine() {
    for (const char* dir : {"/usr/lib/qt6/plugins", "/usr/lib64/qt6/plugins", "/usr/lib/x86_64-linux-gnu/qt6/plugins"})
        if (fs::exists(fs::path(dir) / "platformthemes/libqt6engine-plugin.so"))
            return true;
    return false;
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

void install_qt_theme(bool light, std::string_view accent) {
    if (!has_qtengine())
        return;  // Qt apps still follow the colour scheme through the portal
    if (const char* qpa = std::getenv("QT_QPA_PLATFORMTHEME"); qpa && *qpa && std::string_view(qpa) != "qtengine")
        return;  // the user picked another platform theme
    const fs::path base = data_home();
    if (base.empty())
        return;
    const fs::path dir = base / "atrium/qt";
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
