#include "theme.hpp"

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

void install_gtk_theme(bool light) {
    fs::path base;
    if (const char* d = std::getenv("XDG_DATA_HOME"); d && *d)
        base = d;
    else if (const char* h = std::getenv("HOME"))
        base = fs::path(h) / ".local/share";
    else
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
        ok &= write_if_changed(dir / v / "gtk.css", css);
    }
    if (!ok) {
        wlr_log(WLR_ERROR, "theme: couldn't write %s; GTK apps keep their own buttons", dir.c_str());
        return;
    }
    setenv("GTK_THEME", "atrium", 1);
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
