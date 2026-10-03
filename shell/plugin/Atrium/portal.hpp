#pragma once
// atrium's own xdg-desktop-portal backend (org.freedesktop.impl.portal.
// desktop.atrium), the atrium-portal program (shell/portal), started when
// the portal first asks for it. GlobalShortcuts: an app's shortcuts are rows of atrium's
// shortcut list (action "portal", arg "APP/ID"), so they're kept and can
// be changed in Settings; the compositor says when one is pressed and let go.
// Settings, Wallpaper and Inhibit follow atrium's settings and logind.

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QObject>
#include <QPoint>
#include <QProcess>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace atrium {

// One shortcut as the portal passes them: its id and {description, preferred_trigger}.
struct PortalShortcut {
    QString id;
    QVariantMap options;
};
using PortalShortcuts = QList<PortalShortcut>;

// The Settings portal's accent colour: red, green, blue, 0 to 1 (out of
// that: no preference).
struct PortalColor {
    double r = -1, g = -1, b = -1;
};
using PortalNamespaces = QMap<QString, QVariantMap>;

// Access dialog choices: a(ssa(ss)s) asked, a(ss) answered.
struct PortalPair {
    QString id, label;
};
using PortalPairs = QList<PortalPair>;
struct PortalChoice {
    QString id, label;
    PortalPairs options;  // none: a checkbox, "true" or "false"
    QString initial;
};
using PortalChoices = QList<PortalChoice>;

// A ScreenCast stream as the portal hands it on: (ua{sv}), its PipeWire node
// and {position, size, source_type, id}.
struct PortalStream {
    uint node = 0;
    QVariantMap properties;
};
using PortalStreams = QList<PortalStream>;
// What a ScreenCast session restores the next time (suv): our name, a
// version, and {kind, name}.
struct PortalRestore {
    QString vendor;
    uint version = 0;
    QDBusVariant data;
};

QDBusArgument& operator<<(QDBusArgument& arg, const PortalStream& s);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalStream& s);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalRestore& r);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalRestore& r);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalColor& c);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalColor& c);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalPair& p);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalPair& p);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalChoice& c);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalChoice& c);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalShortcut& s);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalShortcut& s);

// Dialogs are QML files run in their own atrium-shell: the installed ones,
// or the source tree's for a portal run from the build tree.
QString shellFile(const QString& name);
QString shellProgram();
// What goes back to the portal, from a dialog's stdout (nothing when it
// failed or the portal closed it first).
using Answer = std::function<QVariantList(const std::optional<QByteArray>& out)>;
// Runs a shell file (a dialog) for the call being answered, with `input` on
// its stdin and `mode` in ATRIUM_CAPTURE_MODE, until it exits or the portal
// closes the request. `keepInput`: stdin stays open for more (the process
// returned, while it runs).
QProcess* askShell(const QString& handle, const QString& file, const QByteArray& input, const QString& mode,
                   Answer answer, bool keepInput = false);

// A session the portal made for an app: its object, closed by either side.
class PortalSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Session")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    PortalSession(const QString& path, const QString& app, QObject* parent);
    ~PortalSession() override;

    uint version() const { return 1; }
    QString path() const { return path_; }
    QString app() const { return app_; }
    QStringList ids;  // the shortcuts it bound
    QHash<QString, QString> descriptions;  // id → what the app calls it

    // Ends it from this side: the portal (and the app) hear Closed.
    void end();

public slots:
    void Close();

signals:
    void Closed();

private:
    QString path_, app_;
};

class PortalBackend : public QObject, protected QDBusContext {
    Q_OBJECT

public:
    // An error for the D-Bus call being answered (adaptors have no context of their own).
    void fail(const QString& name, const QString& message) {
        if (calledFromDBus())
            sendErrorReply(name, message);
    }

    static PortalBackend* instance();

    // Takes the bus name; false when another backend has it.
    Q_INVOKABLE bool start();
    // The call being answered, to answer later (an adaptor's slot returns first).
    QDBusMessage delayReply() {
        setDelayedReply(true);
        return message();
    }

    PortalSession* session(const QString& path) const { return sessions_.value(path); }
    void addSession(PortalSession* s);
    void dropSession(const QString& path) { sessions_.remove(path); }
    // The app's shortcuts as atrium has them: [{id, keys, row}] by id.
    QHash<QString, QVariantMap> shortcutsOf(const QString& app) const;
    // "Ctrl+Shift+A" from "ctrl+shift+a".
    static QString describe(const QString& keys);

signals:
    void pressed(const QString& app, const QString& id, bool down);

private:
    PortalBackend();
    QHash<QString, PortalSession*> sessions_;
};

class GlobalShortcutsAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.GlobalShortcuts")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit GlobalShortcutsAdaptor(PortalBackend* parent);
    uint version() const { return 2; }

public slots:
    uint CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint BindShortcuts(const QDBusObjectPath& handle, const QDBusObjectPath& session,
                       const atrium::PortalShortcuts& shortcuts, const QString& parentWindow,
                       const QVariantMap& options, QVariantMap& results);
    uint ListShortcuts(const QDBusObjectPath& handle, const QDBusObjectPath& session, QVariantMap& results);
    void ConfigureShortcuts(const QDBusObjectPath& session, const QString& parentWindow, const QVariantMap& options);

signals:
    void Activated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp, const QVariantMap& options);
    void Deactivated(const QDBusObjectPath& session, const QString& id, qulonglong timestamp,
                     const QVariantMap& options);
    void ShortcutsChanged(const QDBusObjectPath& session, const atrium::PortalShortcuts& shortcuts);

private:
    PortalShortcuts listFor(PortalSession* s) const;
    PortalBackend* backend_;
};

// Settings: dark or light, the accent and contrast, from atrium's settings,
// live (org.freedesktop.appearance).
class SettingsAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Settings")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit SettingsAdaptor(PortalBackend* parent);
    uint version() const { return 2; }

public slots:
    atrium::PortalNamespaces ReadAll(const QStringList& namespaces);
    QDBusVariant Read(const QString& ns, const QString& key);

signals:
    void SettingChanged(const QString& ns, const QString& key, const QDBusVariant& value);

private:
    QVariantMap appearance() const;
    QVariantMap last_;
};

// Access: the portal asks the user to allow something (an app setting the
// wallpaper, running in the background). atrium's dialog (access.qml) is
// its own process, handed the question on stdin; it answers on stdout.
class AccessAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Access")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit AccessAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 1; }

public slots:
    uint AccessDialog(const QDBusObjectPath& handle, const QString& app, const QString& parentWindow,
                      const QString& title, const QString& subtitle, const QString& body,
                      const QVariantMap& options, QVariantMap& results);
};

// Screenshot: atrium's screenshot tool (capture.qml), and its colour picker.
class ScreenshotAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Screenshot")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit ScreenshotAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 2; }

public slots:
    uint Screenshot(const QDBusObjectPath& handle, const QString& app, const QString& parentWindow,
                    const QVariantMap& options, QVariantMap& results);
    uint PickColor(const QDBusObjectPath& handle, const QString& app, const QString& parentWindow,
                   const QVariantMap& options, QVariantMap& results);
};

// A question on screen, until answered or the portal closes it.
class PortalRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    PortalRequest(const QString& path, QObject* parent);
    ~PortalRequest() override;

public slots:
    void Close() { emit closed(); }

signals:
    void closed();

private:
    QString path_;
};

// Wallpaper: an app sets the desktop picture. atrium keeps its own copy
// (a sandboxed app's file can vanish) and sets it at once; it has no lock
// screen, so "lockscreen" alone is refused.
class WallpaperAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Wallpaper")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit WallpaperAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 1; }

public slots:
    uint SetWallpaperURI(const QDBusObjectPath& handle, const QString& app, const QString& parentWindow,
                         const QString& uri, const QVariantMap& options);
};

// One app's inhibition, alive until the portal closes its request: idle and
// suspend hold a logind idle lock (atrium itself never sleeps when idle, but
// logind's IdleAction and `systemd-inhibit --list` see it). Logging out and
// switching user aren't held up, as in Plasma.
class InhibitRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    InhibitRequest(const QString& path, QDBusUnixFileDescriptor lock, QObject* parent);
    ~InhibitRequest() override;

public slots:
    void Close();

private:
    QString path_;
    QDBusUnixFileDescriptor lock_;
};

class InhibitAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Inhibit")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit InhibitAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 3; }

public slots:
    void Inhibit(const QDBusObjectPath& handle, const QString& app, const QString& window, uint flags,
                 const QVariantMap& options);
    // The session's state for apps that watch it: always running, never
    // a screensaver (atrium has none).
    uint CreateMonitor(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QString& window, QVariantMap& results);
    void QueryEndResponse(const QDBusObjectPath&) {}

signals:
    void StateChanged(const QDBusObjectPath& session, const QVariantMap& state);
};

// ScreenCast: atrium's streams (src/screencast.hpp), picked with its chooser
// (share.qml) unless the app asks to restore an earlier pick that is still
// there. One source a session.
class ScreenCastAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.ScreenCast")
    Q_PROPERTY(uint AvailableSourceTypes READ sourceTypes CONSTANT)
    Q_PROPERTY(uint AvailableCursorModes READ cursorModes CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit ScreenCastAdaptor(PortalBackend* parent);
    uint sourceTypes() const { return 1 | 2; }   // monitors, windows
    uint cursorModes() const { return 1 | 2 | 4; }  // hidden, embedded, metadata
    uint version() const { return 5; }

    // For RemoteDesktop, whose sessions share the screen too: one of its
    // sessions taken on, whether the app asked for a screen in it, the
    // streams' answer (`request` ends the chooser when it goes), and where a
    // stream's picture starts on the desktop.
    void adopt(const QString& session);
    bool selected(const QString& session) const;
    void startCast(const QString& session, QObject* request, std::function<void(uint, const QVariantMap&)> answer);
    std::optional<QPoint> origin(const QString& session, uint node) const;
    // What RemoteDesktop restores ({kind, name}; none: as it is), and how
    // long to keep it.
    void restoreFrom(const QString& session, const QVariantMap& data, uint persist);

public slots:
    uint CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint SelectSources(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
               const QString& parentWindow, const QVariantMap& options, QVariantMap& results);

private:
    struct Cast {
        uint types = 1;
        uint cursor = 2;
        uint persist = 0;
        bool selected = false;  // SelectSources came
        QVariantMap restore;  // {kind, name} from the app's restore_data
        QList<qint64> streams;
        QHash<uint, QPoint> origins;  // by PipeWire node
    };
    QHash<QString, Cast> casts_;  // by session path
    void end(const QString& session);
};

} // namespace atrium

Q_DECLARE_METATYPE(atrium::PortalStream)
Q_DECLARE_METATYPE(atrium::PortalStreams)
Q_DECLARE_METATYPE(atrium::PortalRestore)
Q_DECLARE_METATYPE(atrium::PortalColor)
Q_DECLARE_METATYPE(atrium::PortalPair)
Q_DECLARE_METATYPE(atrium::PortalPairs)
Q_DECLARE_METATYPE(atrium::PortalChoice)
Q_DECLARE_METATYPE(atrium::PortalChoices)
Q_DECLARE_METATYPE(atrium::PortalNamespaces)
Q_DECLARE_METATYPE(atrium::PortalShortcut)
Q_DECLARE_METATYPE(atrium::PortalShortcuts)
