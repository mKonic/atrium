// The Atrium QML module: C++ helpers for atrium's shell, so its QML stays
// layout and bindings. `import Atrium` in the shell.

#include "apps.hpp"
#include "brightness.hpp"
#include "desktop_files.hpp"
#include "levels.hpp"
#include "session.hpp"
#include "greeter.hpp"
#include "mpris.hpp"
#include "tray.hpp"
#include "audio.hpp"
#include "bluetooth.hpp"
#include "network.hpp"
#include "keyed_model.hpp"
#include "sysinfo.hpp"
#include "emojis.hpp"
#include "netspeed.hpp"
#include "polkit_agent.hpp"
#include "settings_pages.hpp"
#include "accounts.hpp"
#include "requirements.hpp"
#include "wallpaper.hpp"
#include "default_apps.hpp"
#include "keyboard_layouts.hpp"
#include "welcome.hpp"
#include "datetime.hpp"
#include "region.hpp"
#include "autostart.hpp"
#include "battery.hpp"
#include "clipboard.hpp"
#include "recorder.hpp"
#include "compositor.hpp"
#include "notifications.hpp"
#include "notification_server.hpp"
#include "views.hpp"
#include "search.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <vector>

namespace atrium {

// Launcher search, exposed as the `Search` singleton.
class SearchApi : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // 0 is no match; see search::score.
    Q_INVOKABLE int score(const QString& query, const QString& text) const {
        return search::score(query.toLower().toStdString(), text.toLower().toStdString());
    }

    // The result of a calculation as text, or "" when `text` is not one.
    Q_INVOKABLE QString calculate(const QString& text) const {
        const auto v = search::calculate(text.toStdString());
        return v ? QString::fromStdString(search::format_number(*v)) : QString();
    }
};

// Level stepping for the media keys, exposed as the `Levels` singleton.
class LevelsApi : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // `value` one step up or down: 16 steps, or 64 when `fine`.
    Q_INVOKABLE double step(double value, int direction, bool fine) const {
        return levels::step(value, direction, fine ? levels::kFineSteps : levels::kSteps);
    }
};

class AtriumPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void initializeEngine(QQmlEngine* engine, const char*) override {
        if (!engine->imageProvider("notification"))
            engine->addImageProvider("notification", new NotificationImages);
        if (!engine->imageProvider("trayicon"))
            engine->addImageProvider("trayicon", new TrayIcons);
    }

    void registerTypes(const char* uri) override {
        qmlRegisterSingletonType<SearchApi>(uri, 1, 0, "Search", [](QQmlEngine*, QJSEngine*) -> QObject* {
            return new SearchApi;
        });
        // The compositor, as live state: `Atrium.windows`, `Atrium.switchSpace(2)`.
        qmlRegisterSingletonType<Compositor>(uri, 1, 0, "Atrium", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* c = Compositor::instance();
            QQmlEngine::setObjectOwnership(c, QQmlEngine::CppOwnership);
            return c;
        });
        qmlRegisterSingletonType<NotificationHistory>(uri, 1, 0, "NotificationHistory",
            [](QQmlEngine*, QJSEngine*) -> QObject* {
                QObject* o = NotificationHistory::instance();
                QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
                return o;
            });
        // The notification server, as `NotificationServer.popups`.
        qmlRegisterSingletonType<NotificationServer>(uri, 1, 0, "NotificationServer",
            [](QQmlEngine*, QJSEngine*) -> QObject* {
                QObject* o = NotificationServer::instance();
                QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
                return o;
            });
        qmlRegisterUncreatableType<Notification>(uri, 1, 0, "Notification", "from NotificationServer");
        qmlRegisterSingletonType<ClipboardHistory>(uri, 1, 0, "Clipboard",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new ClipboardHistory; });
        qmlRegisterSingletonType<Brightness>(uri, 1, 0, "Brightness",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Brightness; });
        qmlRegisterSingletonType<Recorder>(uri, 1, 0, "Recorder",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Recorder; });
        qmlRegisterSingletonType<LevelsApi>(uri, 1, 0, "Levels", [](QQmlEngine*, QJSEngine*) -> QObject* {
            return new LevelsApi;
        });
        qmlRegisterSingletonType<Session>(uri, 1, 0, "Session",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Session; });
        qmlRegisterSingletonType<SystemInfo>(uri, 1, 0, "SystemInfo",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemInfo; });
        // The polkit agent, as `PolkitAgent.flow`.
        qmlRegisterSingletonType<PolkitAgent>(uri, 1, 0, "PolkitAgent", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = PolkitAgent::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        // Media players: `Mpris.players`, `Mpris.active`.
        qmlRegisterSingletonType<Mpris>(uri, 1, 0, "Mpris", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = Mpris::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterUncreatableType<MprisPlayer>(uri, 1, 0, "MprisPlayer", "from Mpris.players");
        // Sound: `Audio.sink`, `Audio.outputs`, `Audio.apps`.
        qmlRegisterSingletonType<Audio>(uri, 1, 0, "Audio", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = Audio::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterUncreatableType<AudioNode>(uri, 1, 0, "AudioNode", "from Audio");
        // Bluetooth: `Bluetooth.adapter`.
        qmlRegisterSingletonType<Bluetooth>(uri, 1, 0, "Bluetooth", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = Bluetooth::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterUncreatableType<BluetoothAdapter>(uri, 1, 0, "BluetoothAdapter", "Bluetooth.adapter");
        qmlRegisterUncreatableType<BluetoothDevice>(uri, 1, 0, "BluetoothDevice", "from an adapter");
        qmlRegisterUncreatableType<BluetoothDeviceState>(uri, 1, 0, "BluetoothDeviceState", "an enum");
        qmlRegisterType<KeyedModel>(uri, 1, 0, "KeyedModel");
        // Networks: `Network.networks`, `Network.wifiEnabled`.
        qmlRegisterSingletonType<Network>(uri, 1, 0, "Network", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = Network::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterUncreatableType<WifiNetwork>(uri, 1, 0, "WifiNetwork", "from Network.networks");
        qmlRegisterUncreatableType<WiredDevice>(uri, 1, 0, "WiredDevice", "from Network.wired");
        // The system tray: `SystemTray.items`.
        qmlRegisterSingletonType<SystemTray>(uri, 1, 0, "SystemTray", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = SystemTray::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterUncreatableType<SystemTrayItem>(uri, 1, 0, "SystemTrayItem", "from SystemTray.items");
        // greetd, for the login screen.
        qmlRegisterSingletonType<Greeter>(uri, 1, 0, "Greeter", [](QQmlEngine*, QJSEngine*) -> QObject* {
            return new Greeter;
        });
        qmlRegisterUncreatableType<AuthFlow>(uri, 1, 0, "AuthFlow", "from PolkitAgent");
        qmlRegisterUncreatableType<PolkitIdentity>(uri, 1, 0, "PolkitIdentity", "from an AuthFlow");
        qmlRegisterSingletonType<SettingsPages>(uri, 1, 0, "SettingsPages",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new SettingsPages; });
        // What Settings fronts that isn't there: `Requirements.missing[need]`.
        qmlRegisterSingletonType<Requirements>(uri, 1, 0, "Requirements", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = Requirements::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterSingletonType<Wallpaper>(uri, 1, 0, "Wallpaper",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Wallpaper; });
        qmlRegisterSingletonType<DefaultApps>(uri, 1, 0, "DefaultApps",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new DefaultApps; });
        qmlRegisterSingletonType<KeyboardLayouts>(uri, 1, 0, "KeyboardLayouts",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new KeyboardLayouts; });
        qmlRegisterSingletonType<DateTime>(uri, 1, 0, "DateTime",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new DateTime; });
        qmlRegisterSingletonType<Region>(uri, 1, 0, "LocaleSettings",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Region; });
        // The battery: `Battery.present`, `Battery.glyph`, `Battery.remaining`.
        qmlRegisterSingletonType<Battery>(uri, 1, 0, "Battery", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = Battery::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
        qmlRegisterSingletonType<Autostart>(uri, 1, 0, "Autostart",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Autostart; });
        qmlRegisterSingletonType<Welcome>(uri, 1, 0, "Welcome",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Welcome; });
        qmlRegisterSingletonType<Accounts>(uri, 1, 0, "Accounts",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new Accounts; });
        qmlRegisterSingletonType<Emojis>(uri, 1, 0, "Emojis",
                                         [](QQmlEngine*, QJSEngine*) -> QObject* { return new Emojis; });
        qmlRegisterType<NetSpeed>(uri, 1, 0, "NetSpeed");
        qmlRegisterType<OutputState>(uri, 1, 0, "OutputState");
        qmlRegisterType<SpaceWindows>(uri, 1, 0, "SpaceWindows");
        qmlRegisterType<LauncherResults>(uri, 1, 0, "LauncherResults");
        qmlRegisterType<DockApps>(uri, 1, 0, "DockApps");
        qmlRegisterType<DesktopFiles>(uri, 1, 0, "DesktopFiles");
    }
};

} // namespace atrium

#include "plugin.moc"
