// atrium-shell: runs one of atrium's QML shells: the desktop (shell.qml),
// System Settings (settings.qml) or the login screen (greeter.qml).
//
//   atrium-shell [--no-duplicate] [PATH]
//
// PATH is a .qml file, or a directory holding shell.qml (default: the
// installed shell). The directory above the shell's is an import path, so
// its folders import as `shell.modules.bar`. A `//@ pragma AppId ID` line at
// the top of the file names the app. --no-duplicate exits at once when the
// same file already runs for this user.

#include "paths.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>

#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

QString default_shell() {
    for (const QString& dir : {QStringLiteral(ATRIUM_DATADIR "/shell"), QStringLiteral(ATRIUM_SOURCE_DIR "/shell")})
        if (QFileInfo::exists(dir + "/shell.qml"))
            return dir;
    return {};
}

// `//@ pragma NAME VALUE` lines before the first import.
QString pragma(const QString& file, const QString& name) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith("import"))
            break;
        if (!line.startsWith("//@ pragma "))
            continue;
        const QStringList parts = line.mid(11).split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts[0] == name)
            return parts[1];
    }
    return {};
}

// The icon theme when the platform names none (no KDE or qtengine platform
// theme): the one KDE's or GTK's settings name, else a full theme that is
// installed. Qt alone knows only hicolor, where most icons aren't.
void pick_icon_theme() {
    const QString current = QIcon::themeName();
    if (!current.isEmpty() && current != "hicolor")
        return;
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QStringList wanted;
    QSettings kde(config + "/kdeglobals", QSettings::IniFormat);
    wanted << kde.value("Icons/Theme").toString();
    for (const char* gtk : {"/gtk-4.0/settings.ini", "/gtk-3.0/settings.ini"})
        wanted << QSettings(config + gtk, QSettings::IniFormat).value("Settings/gtk-icon-theme-name").toString();
    wanted << "breeze-dark" << "breeze" << "Adwaita" << "Papirus";
    for (const QString& name : wanted) {
        if (name.isEmpty())
            continue;
        for (const QString& dir : QIcon::themeSearchPaths())
            if (QFileInfo::exists(dir + "/" + name + "/index.theme")) {
                QIcon::setThemeName(name);
                return;
            }
    }
}

// Unthemed icons, as the icon theme spec looks them up: loose files in
// ~/.icons, each data dir's icons/ and /usr/share/pixmaps. Qt searches only
// pixmaps, so a distribution logo at /usr/share/icons/<LOGO>.svg (CachyOS's)
// was found only while a platform theme added the rest.
void add_icon_base_dirs() {
    QStringList dirs{QDir::homePath() + "/.icons"};
    for (const QString& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        dirs << data + "/icons";
    for (const QString& dir : QIcon::fallbackSearchPaths())
        if (!dirs.contains(dir))
            dirs << dir;
    QIcon::setFallbackSearchPaths(dirs);
}

// The fonts atrium brings along (Rubik, which Arch only has in the AUR):
// installed, else the source tree's. Given to fontconfig before Qt starts,
// not QFontDatabase::addApplicationFont: that registers a variable font's
// first named instance only (Rubik's Light), so every weight was Light
// with bold faked. fontconfig lists all of them, as a system install does.
void load_fonts() {
    for (const QString& dir : {QStringLiteral(ATRIUM_DATADIR "/fonts"), QStringLiteral(ATRIUM_SOURCE_DIR "/data/share/atrium/fonts")}) {
        if (QDir(dir).entryList({"*.ttf", "*.otf"}, QDir::Files).isEmpty())
            continue;
        FcConfigAppFontAddDir(nullptr, reinterpret_cast<const FcChar8*>(QFile::encodeName(dir).constData()));
        return;
    }
}

// Held for the process's life: a second instance of the file can't take it.
bool first_instance(const QString& file) {
    QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (dir.isEmpty())
        dir = QDir::tempPath();
    const QByteArray hash = QCryptographicHash::hash(file.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    const QByteArray path = (dir + "/atrium-shell-" + hash + ".lock").toLocal8Bit();
    const int fd = open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return true;  // no way to tell: run
    return flock(fd, LOCK_EX | LOCK_NB) == 0;  // fd stays open on purpose
}

} // namespace

// Translations: atrium's own (translations/atrium_<lang>.qm, beside the
// shell) for the system's language, and Qt's for what Qt draws itself
// (dialog buttons). The language is localed's LANG, as Settings > Language
// & Region sets it; the session's LANG until it says.
QTranslator* g_atrium_tr = nullptr;
QTranslator* g_qt_tr = nullptr;

void load_translations(const QString& dir, const QLocale& locale) {
    for (QTranslator** t : {&g_atrium_tr, &g_qt_tr}) {
        if (*t) {
            QCoreApplication::removeTranslator(*t);
            delete *t;
            *t = nullptr;
        }
    }
    QLocale::setDefault(locale);
    auto* own = new QTranslator;
    if (own->load(locale, "atrium", "_", dir))
        QCoreApplication::installTranslator(g_atrium_tr = own);
    else
        delete own;
    auto* qt = new QTranslator;
    if (qt->load(locale, "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(g_qt_tr = qt);
    else
        delete qt;
}

// localed's LANG ("LANG=de_DE.UTF-8"), or nothing when it can't be asked.
QString system_language() {
    QDBusMessage m = QDBusMessage::createMethodCall("org.freedesktop.locale1", "/org/freedesktop/locale1",
                                                    "org.freedesktop.DBus.Properties", "Get");
    m << QStringLiteral("org.freedesktop.locale1") << QStringLiteral("Locale");
    const QDBusMessage r = QDBusConnection::systemBus().call(m, QDBus::Block, 2000);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return {};
    for (const QString& a : r.arguments().first().value<QDBusVariant>().variant().toStringList())
        if (a.startsWith("LANG="))
            return a.mid(5).section('.', 0, 0);
    return {};
}

int main(int argc, char** argv) {
    QString path;
    bool no_duplicate = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-duplicate") || !std::strcmp(argv[i], "-n")) {
            no_duplicate = true;
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::puts("usage: atrium-shell [--no-duplicate] [PATH]");
            return 0;
        } else {
            path = QString::fromLocal8Bit(argv[i]);
        }
    }
    if (path.isEmpty())
        path = default_shell();
    QFileInfo info(path);
    if (info.isDir())
        info = QFileInfo(info.absoluteFilePath() + "/shell.qml");
    if (!info.exists()) {
        std::fprintf(stderr, "atrium-shell: no shell at %s\n", qPrintable(path));
        return 1;
    }
    const QString file = info.absoluteFilePath();
    if (no_duplicate && !first_instance(file))
        return 0;

    // Panels are see-through: every window gets an alpha channel.
    QQuickWindow::setDefaultAlphaBuffer(true);
    load_fonts();  // before Qt reads fontconfig's font list
    QApplication app(argc, argv);
    app.setApplicationName("atrium-shell");
    const QString app_id = pragma(file, "AppId");
    app.setDesktopFileName(app_id.isEmpty() ? QStringLiteral("atrium-shell") : app_id);
    // Closing Settings' window must not end the desktop's process; each
    // file decides when it quits.
    app.setQuitOnLastWindowClosed(false);
    app.setProperty("atriumShellDir", info.absolutePath());
    pick_icon_theme();
    add_icon_base_dirs();

    QQmlEngine engine;
    // The Atrium modules: built next to this binary when it runs from the
    // build tree, else installed.
    const QString built = QStringLiteral(ATRIUM_BUILD_DIR "/shell/plugin");
    if (QCoreApplication::applicationFilePath().startsWith(ATRIUM_BUILD_DIR "/") && QFileInfo::exists(built + "/Atrium/Shell/qmldir"))
        engine.addImportPath(built);
    else
        engine.addImportPath(QStringLiteral(ATRIUM_QML_DIR));
    if (qEnvironmentVariableIsSet("ATRIUM_SHELL_DEBUG"))
        std::fprintf(stderr, "import paths: %s\n", qPrintable(engine.importPathList().join(" ")));
    QDir above(info.absolutePath());
    above.cdUp();
    engine.addImportPath(above.absolutePath());
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QCoreApplication::quit);
    QObject::connect(&engine, &QQmlEngine::exit, &app, &QCoreApplication::exit);

    // The session's language, then localed's as it changes (Settings), live.
    const QString translations = QDir(info.absolutePath()).absoluteFilePath("../translations");
    load_translations(translations, QLocale::system());
    // localed says something changed: a timer's slot takes the signal (this
    // file has no moc), and its timeout reloads when LANG is new.
    auto* relang = new QTimer(&app);
    relang->setSingleShot(true);
    QObject::connect(relang, &QTimer::timeout, &app, [translations, &engine, last = QLocale::system().name()]() mutable {
        const QString lang = system_language();
        if (lang.isEmpty() || lang == last)
            return;
        last = lang;
        load_translations(translations, QLocale(lang));
        engine.retranslate();
    });
    QDBusConnection::systemBus().connect("org.freedesktop.locale1", "/org/freedesktop/locale1",
                                         "org.freedesktop.DBus.Properties", "PropertiesChanged", relang,
                                         SLOT(start()));

    QQmlComponent component(&engine, QUrl::fromLocalFile(file));
    if (component.isError()) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    QObject* root = component.create();
    if (!root) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }
    const int code = app.exec();
    delete root;
    return code;
}
