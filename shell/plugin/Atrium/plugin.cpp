// The Atrium QML module: C++ helpers for atrium's shell, so its QML stays
// layout and bindings. `import Atrium` in the shell.

#include "apps.hpp"
#include "brightness.hpp"
#include "desktop_files.hpp"
#include "levels.hpp"
#include "session.hpp"
#include "sysinfo.hpp"
#include "netspeed.hpp"
#include "admin_cache.hpp"
#include "settings_pages.hpp"
#include "clipboard.hpp"
#include "recorder.hpp"
#include "compositor.hpp"
#include "notifications.hpp"
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
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new NotificationHistory; });
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
        qmlRegisterSingletonType<AdminCache>(uri, 1, 0, "AdminCache",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new AdminCache; });
        qmlRegisterSingletonType<SettingsPages>(uri, 1, 0, "SettingsPages",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new SettingsPages; });
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
