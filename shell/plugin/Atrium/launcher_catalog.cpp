#include "launcher_catalog.hpp"

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
    {"screen:clipboard", "Clipboard History", "content_paste", "copy paste history"},
    {"screen:emoji", "Search Emoji & Symbols", "mood", "emoji symbols smiley picker"},
    {"screen:files", "Search Files", "folder_open", "find file folder document locate"},
    {"screen:windows", "Switch Windows", "select_window", "window switch focus alt tab"},
    {"screen:snippets", "Search Snippets", "text_snippet", "snippet text template"},
    {"screen:quicklinks", "Search Quicklinks", "link", "quicklink bookmark url"},
    {"screen:calculator", "Calculator History", "calculate", "calculator math history"},
    {"new-note", "New Note", "note_add", "create note write"},
    {"screen:notes", "Search Notes", "manage_search", "find notes"},
    {"create-quicklink", "Create Quicklink", "add_link", "new quicklink bookmark"},
    {"create-snippet", "Create Snippet", "note_add", "new snippet text"},
    {"create-command", "Create Command", "terminal", "new custom command script shell"},
    {"launcher-settings", "Launcher Settings", "tune", "palette launcher preferences aliases"},
    {"screenshot", "Take Screenshot", "screenshot_region", "capture screen grab print"},
    {"record", "Record Screen", "screen_record", "recording video capture"},
    {"overview", "Overview", "overview_key", "mission control expose spaces windows"},
    {"notifications", "Notification Center", "notifications", "notifications history"},
    {"control", "Control Center", "toggle_on", "control center quick settings"},
    {"welcome", "Welcome to atrium", "waving_hand", "tour introduction"},
    {"reset-ranking", "Reset Learned Ranking", "restart_alt", "frecency forget history", true},
};

constexpr CatalogItem kSystem[] = {
    {"sleep", "Sleep", "bedtime", "suspend standby"},
    {"restart", "Restart", "restart_alt", "reboot"},
    {"shutdown", "Shut Down", "power_settings_new", "power off poweroff halt"},
    {"logout", "Log Out", "logout", "sign out exit session"},
    {"quit-all", "Quit All Apps", "cancel_presentation", "close everything windows", true},
    {"empty-trash", "Empty Trash", "delete_forever", "bin recycle", true},
    {"open-trash", "Open Trash", "delete", "bin recycle"},
    {"toggle-appearance", "Toggle System Appearance", "contrast", "dark light mode theme"},
    {"dark", "Dark Appearance", "dark_mode", "dark mode theme"},
    {"light", "Light Appearance", "light_mode", "light mode theme"},
    {"toggle-bluetooth", "Toggle Bluetooth", "bluetooth", "bt wireless"},
    {"toggle-wifi", "Toggle Wi-Fi", "wifi", "wireless network wlan"},
    {"toggle-vpn", "Toggle VPN", "vpn_key", "wireguard mullvad tunnel"},
    {"toggle-mute", "Toggle Mute", "volume_off", "sound audio silence"},
    {"volume-up", "Volume Up", "volume_up", "sound louder"},
    {"volume-down", "Volume Down", "volume_down", "sound quieter"},
    {"toggle-mic", "Toggle Microphone Mute", "mic_off", "microphone input"},
    {"play-pause", "Play / Pause", "play_pause", "media music"},
    {"next-track", "Next Track", "skip_next", "media music"},
    {"previous-track", "Previous Track", "skip_previous", "media music"},
    {"toggle-night-light", "Toggle Night Light", "nightlight", "warm blue light"},
    {"toggle-dnd", "Toggle Do Not Disturb", "do_not_disturb_on", "focus quiet notifications"},
    {"dismiss-notifications", "Dismiss Notifications", "clear_all", "clear notifications"},
    {"eject-all", "Eject All Disks", "eject", "unmount usb drives"},
    {"toggle-tiling", "Toggle Tiling", "view_quilt", "tile dwindle layout"},
};

constexpr CatalogItem kWindow[] = {
    {"left-half", "Left Half", "splitscreen_left", "snap"},
    {"right-half", "Right Half", "splitscreen_right", "snap"},
    {"top-half", "Top Half", "splitscreen_top", "snap"},
    {"bottom-half", "Bottom Half", "splitscreen_bottom", "snap"},
    {"center-half", "Center Half", "width_normal", "snap"},
    {"top-left", "Top Left Quarter", "picture_in_picture", "corner snap"},
    {"top-right", "Top Right Quarter", "picture_in_picture", "corner snap"},
    {"bottom-left", "Bottom Left Quarter", "picture_in_picture", "corner snap"},
    {"bottom-right", "Bottom Right Quarter", "picture_in_picture", "corner snap"},
    {"first-third", "First Third", "view_column", "left"},
    {"center-third", "Center Third", "view_column", "middle"},
    {"last-third", "Last Third", "view_column", "right"},
    {"first-two-thirds", "First Two Thirds", "view_column_2", "left"},
    {"center-two-thirds", "Center Two Thirds", "view_column_2", "middle"},
    {"last-two-thirds", "Last Two Thirds", "view_column_2", "right"},
    {"first-fourth", "First Fourth", "view_week", "left quarter column"},
    {"second-fourth", "Second Fourth", "view_week", "quarter column"},
    {"third-fourth", "Third Fourth", "view_week", "quarter column"},
    {"last-fourth", "Last Fourth", "view_week", "right quarter column"},
    {"first-three-fourths", "First Three Fourths", "view_week", "left"},
    {"last-three-fourths", "Last Three Fourths", "view_week", "right"},
    {"top-left-sixth", "Top Left Sixth", "grid_view", ""},
    {"top-center-sixth", "Top Center Sixth", "grid_view", ""},
    {"top-right-sixth", "Top Right Sixth", "grid_view", ""},
    {"bottom-left-sixth", "Bottom Left Sixth", "grid_view", ""},
    {"bottom-center-sixth", "Bottom Center Sixth", "grid_view", ""},
    {"bottom-right-sixth", "Bottom Right Sixth", "grid_view", ""},
    {"maximize", "Maximize", "fullscreen", "zoom full"},
    {"almost-maximize", "Almost Maximize", "fit_screen", "big"},
    {"maximize-height", "Maximize Height", "height", "tall vertical"},
    {"maximize-width", "Maximize Width", "width", "wide horizontal"},
    {"larger", "Make Larger", "zoom_out_map", "grow bigger"},
    {"smaller", "Make Smaller", "zoom_in_map", "shrink"},
    {"center", "Center", "center_focus_strong", "middle"},
    {"restore", "Restore", "restore_page", "undo unsnap"},
    {"move-left", "Move Left", "align_horizontal_left", "edge nudge"},
    {"move-right", "Move Right", "align_horizontal_right", "edge nudge"},
    {"move-up", "Move Up", "align_vertical_top", "edge nudge"},
    {"move-down", "Move Down", "align_vertical_bottom", "edge nudge"},
    {"next-display", "Next Display", "desktop_windows", "monitor screen move"},
    {"save-layout", "Save Window Layout", "dashboard_customize", "arrangement windows remember"},
    {"prev-display", "Previous Display", "desktop_windows", "monitor screen move"},
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
