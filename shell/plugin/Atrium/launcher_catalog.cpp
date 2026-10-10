#include "launcher_catalog.hpp"

#include <QCoreApplication>

#include "bluetooth.hpp"
#include "compositor.hpp"
#include "disks.hpp"
#include "network.hpp"
#include "vpn.hpp"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>

namespace atrium {

namespace {

constexpr CatalogItem kCommands[] = {
    {"screen:clipboard", QT_TRANSLATE_NOOP("launcher", "Clipboard History"), "content_paste", "copy paste history"},
    {"screen:emoji", QT_TRANSLATE_NOOP("launcher", "Search Emoji & Symbols"), "mood", "emoji symbols smiley picker"},
    {"screen:files", QT_TRANSLATE_NOOP("launcher", "Search Files"), "folder_open", "find file folder document locate"},
    {"screen:windows", QT_TRANSLATE_NOOP("launcher", "Switch Windows"), "select_window", "window switch focus alt tab"},
    {"screen:snippets", QT_TRANSLATE_NOOP("launcher", "Search Snippets"), "text_snippet", "snippet text template"},
    {"screen:quicklinks", QT_TRANSLATE_NOOP("launcher", "Search Quicklinks"), "link", "quicklink bookmark url"},
    {"screen:calculator", QT_TRANSLATE_NOOP("launcher", "Calculator History"), "calculate", "calculator math history"},
    {"new-note", QT_TRANSLATE_NOOP("launcher", "New Note"), "note_add", "create note write"},
    {"screen:notes", QT_TRANSLATE_NOOP("launcher", "Search Notes"), "manage_search", "find notes"},
    {"create-quicklink", QT_TRANSLATE_NOOP("launcher", "Create Quicklink"), "add_link", "new quicklink bookmark"},
    {"create-snippet", QT_TRANSLATE_NOOP("launcher", "Create Snippet"), "note_add", "new snippet text"},
    {"create-command", QT_TRANSLATE_NOOP("launcher", "Create Command"), "terminal", "new custom command script shell"},
    {"launcher-settings", QT_TRANSLATE_NOOP("launcher", "Launcher Settings"), "tune", "palette launcher preferences aliases"},
    {"screenshot", QT_TRANSLATE_NOOP("launcher", "Take Screenshot"), "screenshot_region", "capture screen grab print"},
    {"record", QT_TRANSLATE_NOOP("launcher", "Record Screen"), "screen_record", "recording video capture"},
    {"overview", QT_TRANSLATE_NOOP("launcher", "Overview"), "overview_key", "mission control expose spaces windows"},
    {"notifications", QT_TRANSLATE_NOOP("launcher", "Notification Center"), "notifications", "notifications history"},
    {"control", QT_TRANSLATE_NOOP("launcher", "Control Center"), "toggle_on", "control center quick settings"},
    {"welcome", QT_TRANSLATE_NOOP("launcher", "Welcome to atrium"), "waving_hand", "tour introduction"},
    {"reset-ranking", QT_TRANSLATE_NOOP("launcher", "Reset Learned Ranking"), "restart_alt", "frecency forget history", true},
};

constexpr CatalogItem kSystem[] = {
    {"sleep", QT_TRANSLATE_NOOP("launcher", "Sleep"), "bedtime", "suspend standby"},
    {"restart", QT_TRANSLATE_NOOP("launcher", "Restart"), "restart_alt", "reboot"},
    {"shutdown", QT_TRANSLATE_NOOP("launcher", "Shut Down"), "power_settings_new", "power off poweroff halt"},
    {"logout", QT_TRANSLATE_NOOP("launcher", "Log Out"), "logout", "sign out exit session"},
    {"quit-all", QT_TRANSLATE_NOOP("launcher", "Quit All Apps"), "cancel_presentation", "close everything windows", true},
    {"empty-trash", QT_TRANSLATE_NOOP("launcher", "Empty Trash"), "delete_forever", "bin recycle", true},
    {"open-trash", QT_TRANSLATE_NOOP("launcher", "Open Trash"), "delete", "bin recycle"},
    {"toggle-appearance", QT_TRANSLATE_NOOP("launcher", "Toggle System Appearance"), "contrast", "dark light mode theme"},
    {"dark", QT_TRANSLATE_NOOP("launcher", "Dark Appearance"), "dark_mode", "dark mode theme"},
    {"light", QT_TRANSLATE_NOOP("launcher", "Light Appearance"), "light_mode", "light mode theme"},
    {"toggle-bluetooth", QT_TRANSLATE_NOOP("launcher", "Toggle Bluetooth"), "bluetooth", "bt wireless"},
    {"toggle-wifi", QT_TRANSLATE_NOOP("launcher", "Toggle Wi-Fi"), "wifi", "wireless network wlan"},
    {"toggle-vpn", QT_TRANSLATE_NOOP("launcher", "Toggle VPN"), "vpn_key", "wireguard mullvad tunnel"},
    {"toggle-mute", QT_TRANSLATE_NOOP("launcher", "Toggle Mute"), "volume_off", "sound audio silence"},
    {"volume-up", QT_TRANSLATE_NOOP("launcher", "Volume Up"), "volume_up", "sound louder"},
    {"volume-down", QT_TRANSLATE_NOOP("launcher", "Volume Down"), "volume_down", "sound quieter"},
    {"toggle-mic", QT_TRANSLATE_NOOP("launcher", "Toggle Microphone Mute"), "mic_off", "microphone input"},
    {"play-pause", QT_TRANSLATE_NOOP("launcher", "Play / Pause"), "play_pause", "media music"},
    {"next-track", QT_TRANSLATE_NOOP("launcher", "Next Track"), "skip_next", "media music"},
    {"previous-track", QT_TRANSLATE_NOOP("launcher", "Previous Track"), "skip_previous", "media music"},
    {"toggle-night-light", QT_TRANSLATE_NOOP("launcher", "Toggle Night Light"), "nightlight", "warm blue light"},
    {"toggle-dnd", QT_TRANSLATE_NOOP("launcher", "Toggle Do Not Disturb"), "do_not_disturb_on", "focus quiet notifications"},
    {"dismiss-notifications", QT_TRANSLATE_NOOP("launcher", "Dismiss Notifications"), "clear_all", "clear notifications"},
    {"eject-all", QT_TRANSLATE_NOOP("launcher", "Eject All Disks"), "eject", "unmount usb drives"},
    {"toggle-tiling", QT_TRANSLATE_NOOP("launcher", "Toggle Tiling"), "view_quilt", "tile dwindle layout"},
};

constexpr CatalogItem kWindow[] = {
    {"left-half", QT_TRANSLATE_NOOP("launcher", "Left Half"), "splitscreen_left", "snap"},
    {"right-half", QT_TRANSLATE_NOOP("launcher", "Right Half"), "splitscreen_right", "snap"},
    {"top-half", QT_TRANSLATE_NOOP("launcher", "Top Half"), "splitscreen_top", "snap"},
    {"bottom-half", QT_TRANSLATE_NOOP("launcher", "Bottom Half"), "splitscreen_bottom", "snap"},
    {"center-half", QT_TRANSLATE_NOOP("launcher", "Center Half"), "width_normal", "snap"},
    {"top-left", QT_TRANSLATE_NOOP("launcher", "Top Left Quarter"), "picture_in_picture", "corner snap"},
    {"top-right", QT_TRANSLATE_NOOP("launcher", "Top Right Quarter"), "picture_in_picture", "corner snap"},
    {"bottom-left", QT_TRANSLATE_NOOP("launcher", "Bottom Left Quarter"), "picture_in_picture", "corner snap"},
    {"bottom-right", QT_TRANSLATE_NOOP("launcher", "Bottom Right Quarter"), "picture_in_picture", "corner snap"},
    {"first-third", QT_TRANSLATE_NOOP("launcher", "First Third"), "view_column", "left"},
    {"center-third", QT_TRANSLATE_NOOP("launcher", "Center Third"), "view_column", "middle"},
    {"last-third", QT_TRANSLATE_NOOP("launcher", "Last Third"), "view_column", "right"},
    {"first-two-thirds", QT_TRANSLATE_NOOP("launcher", "First Two Thirds"), "view_column_2", "left"},
    {"center-two-thirds", QT_TRANSLATE_NOOP("launcher", "Center Two Thirds"), "view_column_2", "middle"},
    {"last-two-thirds", QT_TRANSLATE_NOOP("launcher", "Last Two Thirds"), "view_column_2", "right"},
    {"first-fourth", QT_TRANSLATE_NOOP("launcher", "First Fourth"), "view_week", "left quarter column"},
    {"second-fourth", QT_TRANSLATE_NOOP("launcher", "Second Fourth"), "view_week", "quarter column"},
    {"third-fourth", QT_TRANSLATE_NOOP("launcher", "Third Fourth"), "view_week", "quarter column"},
    {"last-fourth", QT_TRANSLATE_NOOP("launcher", "Last Fourth"), "view_week", "right quarter column"},
    {"first-three-fourths", QT_TRANSLATE_NOOP("launcher", "First Three Fourths"), "view_week", "left"},
    {"last-three-fourths", QT_TRANSLATE_NOOP("launcher", "Last Three Fourths"), "view_week", "right"},
    {"top-left-sixth", QT_TRANSLATE_NOOP("launcher", "Top Left Sixth"), "grid_view", ""},
    {"top-center-sixth", QT_TRANSLATE_NOOP("launcher", "Top Center Sixth"), "grid_view", ""},
    {"top-right-sixth", QT_TRANSLATE_NOOP("launcher", "Top Right Sixth"), "grid_view", ""},
    {"bottom-left-sixth", QT_TRANSLATE_NOOP("launcher", "Bottom Left Sixth"), "grid_view", ""},
    {"bottom-center-sixth", QT_TRANSLATE_NOOP("launcher", "Bottom Center Sixth"), "grid_view", ""},
    {"bottom-right-sixth", QT_TRANSLATE_NOOP("launcher", "Bottom Right Sixth"), "grid_view", ""},
    {"maximize", QT_TRANSLATE_NOOP("launcher", "Maximize"), "fullscreen", "zoom full"},
    {"almost-maximize", QT_TRANSLATE_NOOP("launcher", "Almost Maximize"), "fit_screen", "big"},
    {"maximize-height", QT_TRANSLATE_NOOP("launcher", "Maximize Height"), "height", "tall vertical"},
    {"maximize-width", QT_TRANSLATE_NOOP("launcher", "Maximize Width"), "width", "wide horizontal"},
    {"larger", QT_TRANSLATE_NOOP("launcher", "Make Larger"), "zoom_out_map", "grow bigger"},
    {"smaller", QT_TRANSLATE_NOOP("launcher", "Make Smaller"), "zoom_in_map", "shrink"},
    {"center", QT_TRANSLATE_NOOP("launcher", "Center"), "center_focus_strong", "middle"},
    {"restore", QT_TRANSLATE_NOOP("launcher", "Restore"), "restore_page", "undo unsnap"},
    {"move-left", QT_TRANSLATE_NOOP("launcher", "Move Left"), "align_horizontal_left", "edge nudge"},
    {"move-right", QT_TRANSLATE_NOOP("launcher", "Move Right"), "align_horizontal_right", "edge nudge"},
    {"move-up", QT_TRANSLATE_NOOP("launcher", "Move Up"), "align_vertical_top", "edge nudge"},
    {"move-down", QT_TRANSLATE_NOOP("launcher", "Move Down"), "align_vertical_bottom", "edge nudge"},
    {"next-display", QT_TRANSLATE_NOOP("launcher", "Next Display"), "desktop_windows", "monitor screen move"},
    {"save-layout", QT_TRANSLATE_NOOP("launcher", "Save Window Layout"), "dashboard_customize", "arrangement windows remember"},
    {"prev-display", QT_TRANSLATE_NOOP("launcher", "Previous Display"), "desktop_windows", "monitor screen move"},
};

void shell(const QString& name) {
    Compositor::instance()->action("shell", name);
}

QString emptyTrash(bool* noop) {
    const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/files";
    const bool empty = QDir(trash).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    // gio empties every trash (other disks' .Trash-UID too), as file managers do.
    if (!QStandardPaths::findExecutable("gio").isEmpty()) {
        QProcess::startDetached("gio", {"trash", "--empty"});
    } else if (!empty) {
        QDir(trash).removeRecursively();
        QDir(trash + "/../info").removeRecursively();
        QDir().mkpath(trash);
        QDir().mkpath(trash + "/../info");
    }
    *noop = empty;
    return empty ? "Trash Is Already Empty" : "Trash Emptied";
}

} // namespace

std::span<const CatalogItem> paletteCommands() {
    return kCommands;
}

std::span<const CatalogItem> systemActions() {
    return kSystem;
}

std::span<const CatalogItem> windowCommands() {
    return kWindow;
}

QString runSystemAction(const QString& id, bool* noop) {
    bool dummy = false;
    if (!noop)
        noop = &dummy;
    *noop = false;
    Compositor* c = Compositor::instance();
    auto setting = [c](const char* key) { return c->settings().value(key); };

    // Power: the shell's session confirm counts down restart and shut down.
    if (id == "sleep" || id == "restart" || id == "shutdown" || id == "logout") {
        shell("session:" + id);
        return {};
    }
    if (id == "empty-trash")
        return emptyTrash(noop);
    if (id == "quit-all") {
        int n = 0;
        for (const QVariant& v : c->windows())
            if (!v.toMap().value("skip_taskbar").toBool()) {
                c->closeWindow(v.toMap().value("id").toInt());
                ++n;
            }
        *noop = n == 0;
        return n == 0 ? QStringLiteral("No Apps to Quit") : QString();
    }
    if (id == "open-trash") {
        QProcess::startDetached("xdg-open", {"trash:///"});
        return {};
    }
    if (id == "toggle-appearance" || id == "dark" || id == "light") {
        const bool light = setting("appearance.style").toString() == "light";
        const bool want = id == "toggle-appearance" ? !light : id == "light";
        *noop = want == light;
        c->setSetting("appearance.style", want ? "light" : "dark");
        return want ? "Light Appearance" : "Dark Appearance";
    }
    if (id == "toggle-bluetooth") {
        BluetoothAdapter* a = Bluetooth::instance()->adapter();
        if (!a) {
            *noop = true;
            return "No Bluetooth Adapter";
        }
        const bool on = !a->enabled();
        a->setEnabled(on);
        return on ? "Bluetooth On" : "Bluetooth Off";
    }
    if (id == "toggle-wifi") {
        Network* n = Network::instance();
        if (!n->available()) {
            *noop = true;
            return "NetworkManager Isn't Running";
        }
        const bool on = !n->wifiEnabled();
        n->setWifiEnabled(on);
        return on ? "Wi-Fi On" : "Wi-Fi Off";
    }
    if (id == "toggle-vpn") {
        VpnTunnel* t = Vpn::instance()->current();
        if (!t) {
            *noop = true;
            return "No VPN Set Up";
        }
        const bool on = !(t->connected() || t->busy());
        t->toggle();
        return on ? t->name() + " On" : t->name() + " Off";
    }
    // The media keys' own handlers show the volume and brightness OSD.
    if (id == "toggle-mute") return shell("volume-mute"), QString();
    if (id == "volume-up") return shell("volume-up"), QString();
    if (id == "volume-down") return shell("volume-down"), QString();
    if (id == "toggle-mic") return shell("mic-mute"), QString();
    if (id == "play-pause") return shell("media-play-pause"), QString();
    if (id == "next-track") return shell("media-next"), QString();
    if (id == "previous-track") return shell("media-previous"), QString();
    if (id == "toggle-night-light") {
        const bool on = !c->nightLight().value("active").toBool();
        c->setNightLight(on);
        return on ? "Night Light On" : "Night Light Off";
    }
    if (id == "toggle-dnd") {
        const bool on = !setting("notifications.dnd").toBool();
        c->setSetting("notifications.dnd", on);
        return on ? "Do Not Disturb On" : "Do Not Disturb Off";
    }
    if (id == "dismiss-notifications") {
        shell("notifications-clear");
        return "Notifications Dismissed";
    }
    if (id == "eject-all") {
        const QVariantList disks = Disks::instance()->ejectable();
        if (disks.isEmpty()) {
            *noop = true;
            return "No Disks to Eject";
        }
        for (const QVariant& d : disks)
            Disks::instance()->eject(d.toMap().value("path").toString());
        return disks.size() == 1 ? QString("Disk Ejected") : QString("%1 Disks Ejected").arg(disks.size());
    }
    if (id == "toggle-tiling") {
        c->action("toggle-tiling");
        return {};
    }
    *noop = true;
    return {};
}

} // namespace atrium
