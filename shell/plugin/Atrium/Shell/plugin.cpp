// The Atrium.Shell QML module: what a shell needs from its host, in place of
// Quickshell's runtime. Windows on the layer shell, the screens, installed
// apps, theme icons, the clock. `import Atrium.Shell` in the shell.

#include "desktop_entries.hpp"
#include "screens.hpp"
#include "scope.hpp"
#include "shell_api.hpp"
#include "system_clock.hpp"
#include "variants.hpp"
#include "windows.hpp"

#include <QIcon>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QQuickImageProvider>

namespace atrium::shell {

// "image://icon/name": the theme's icon at the size asked for.
class IconProvider : public QQuickImageProvider {
public:
    IconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requested) override {
        const QSize want = requested.isValid() && !requested.isEmpty() ? requested : QSize(64, 64);
        QIcon icon = QIcon::fromTheme(id);
        if (icon.isNull())
            icon = QIcon::fromTheme("image-missing");
        const QPixmap p = icon.pixmap(want);
        if (size)
            *size = p.size();
        return p;
    }
};

class ShellPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void initializeEngine(QQmlEngine* engine, const char*) override {
        if (!engine->imageProvider("icon"))
            engine->addImageProvider("icon", new IconProvider);
    }

    void registerTypes(const char* uri) override {
        qmlRegisterType<ShellRoot>(uri, 1, 0, "ShellRoot");
        qmlRegisterType<Scope>(uri, 1, 0, "Scope");
        qmlRegisterType<Singleton>(uri, 1, 0, "Singleton");
        qmlRegisterType<Variants>(uri, 1, 0, "Variants");
        qmlRegisterType<SystemClock>(uri, 1, 0, "SystemClock");
        qmlRegisterType<PanelWindow>(uri, 1, 0, "PanelWindow");
        qmlRegisterType<FloatingWindow>(uri, 1, 0, "FloatingWindow");
        qmlRegisterType<Region>(uri, 1, 0, "Region");
        qmlRegisterUncreatableType<ShellScreen>(uri, 1, 0, "ShellScreen", "one per output, from Shell.screens");
        qmlRegisterUncreatableType<ShellWindow>(uri, 1, 0, "ShellWindow", "PanelWindow or FloatingWindow");
        qmlRegisterUncreatableType<PanelEdges>(uri, 1, 0, "PanelEdges", "a PanelWindow's anchors");
        qmlRegisterUncreatableType<PanelMargins>(uri, 1, 0, "PanelMargins", "a PanelWindow's margins");
        qmlRegisterUncreatableType<LayerSettings>(uri, 1, 0, "LayerSettings", "attached: WlrLayershell");
        qmlRegisterUncreatableType<WlrLayershell>(uri, 1, 0, "WlrLayershell", "attached properties only");
        qmlRegisterUncreatableType<WlrLayer>(uri, 1, 0, "WlrLayer", "an enum");
        qmlRegisterUncreatableType<WlrKeyboardFocus>(uri, 1, 0, "WlrKeyboardFocus", "an enum");
        qmlRegisterUncreatableType<ExclusionMode>(uri, 1, 0, "ExclusionMode", "an enum");
        qmlRegisterUncreatableType<DesktopEntry>(uri, 1, 0, "DesktopEntry", "from DesktopEntries");
        qmlRegisterUncreatableType<DesktopAction>(uri, 1, 0, "DesktopAction", "from a DesktopEntry");
        qmlRegisterUncreatableType<EntryList>(uri, 1, 0, "EntryList", "DesktopEntries.applications");
        qmlRegisterSingletonType<ShellApi>(uri, 1, 0, "Shell", [](QQmlEngine*, QJSEngine*) -> QObject* {
            return new ShellApi;
        });
        qmlRegisterSingletonType<DesktopEntries>(uri, 1, 0, "DesktopEntries", [](QQmlEngine*, QJSEngine*) -> QObject* {
            QObject* o = DesktopEntries::instance();
            QQmlEngine::setObjectOwnership(o, QQmlEngine::CppOwnership);
            return o;
        });
    }
};

} // namespace atrium::shell

#include "plugin.moc"
