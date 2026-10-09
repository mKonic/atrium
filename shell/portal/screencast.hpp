#pragma once
// org.freedesktop.impl.portal.ScreenCast: atrium shares a screen or a window
// itself (CastStream) instead of xdg-desktop-portal-wlr. The app's choice is
// asked for at Start, in atrium's chooser (share.qml) made a child of the
// app's window; an app that asks to keep it gets restore data that shares
// the same screen, or the same app's window, next time without asking.

#include "cast_core.hpp"
#include "cast_stream.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QPointer>
#include <QVariantMap>

#include <optional>

namespace atrium {

class PortalBackend;
class PortalSession;

// restore_data: (suv).
struct RestoreData {
    QString vendor;
    uint version = 0;
    QDBusVariant data;
};
// One stream as Start answers it: (ua{sv}).
struct CastStreamInfo {
    uint node = 0;
    QVariantMap props;
};
using CastStreamInfos = QList<CastStreamInfo>;

QDBusArgument& operator<<(QDBusArgument& arg, const RestoreData& r);
const QDBusArgument& operator>>(const QDBusArgument& arg, RestoreData& r);
QDBusArgument& operator<<(QDBusArgument& arg, const CastStreamInfo& s);
const QDBusArgument& operator>>(const QDBusArgument& arg, CastStreamInfo& s);

// What a session wants shared and what it got, kept on its PortalSession.
class CastState : public QObject {
    Q_OBJECT

public:
    explicit CastState(PortalSession* session);
    static CastState* of(PortalSession* session);  // made on first use

    uint types = cast::Monitor;
    uint cursor = cast::Embedded;
    uint persist = cast::PersistNone;
    bool selected = false;  // SelectSources was called
    std::optional<cast::Choice> restored;
    cast::Choice chosen;
    QPointer<CastStream> stream;
};

class ScreenCastAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.ScreenCast")
    Q_PROPERTY(uint AvailableSourceTypes READ sourceTypes CONSTANT)
    Q_PROPERTY(uint AvailableCursorModes READ cursorModes CONSTANT)
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit ScreenCastAdaptor(PortalBackend* backend);

    uint sourceTypes() const { return cast::Monitor | cast::Window; }
    uint cursorModes() const { return cast::Hidden | cast::Embedded; }
    uint version() const { return 5; }

    // Shared with RemoteDesktop.Start. The chooser's input: a line for each
    // screen and window on offer, as xdg-desktop-portal-wlr gave them.
    static QByteArray offer(uint types);
    // What the chooser answered, as a choice (nothing: the user said no).
    static std::optional<cast::Choice> chosen(const QByteArray& out, bool cursor);
    // What a remembered choice means now, if it's still there.
    static std::optional<cast::Choice> resolve(const cast::Choice& c);
    // Restore data: the a{sv} inside ours ("atrium", 1, a{sv}), and back.
    static std::optional<QVariantMap> restored(const QVariant& restoreData);
    static QVariant restoreData(const QVariantMap& m);
    static QVariantMap encode(const cast::Choice& c);
    static std::optional<cast::Choice> decode(const QVariantMap& m);
    // Starts the session's stream for `choice`; Start's results (the
    // streams, restore data), or nothing when it couldn't.
    static std::optional<QVariantMap> begin(PortalSession* session, const cast::Choice& choice);

public slots:
    uint CreateSession(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint SelectSources(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
                       const QVariantMap& options, QVariantMap& results);
    uint Start(const QDBusObjectPath& handle, const QDBusObjectPath& session, const QString& app,
               const QString& parentWindow, const QVariantMap& options, QVariantMap& results);

private:
    PortalBackend* backend_;
};

} // namespace atrium

Q_DECLARE_METATYPE(atrium::RestoreData)
Q_DECLARE_METATYPE(atrium::CastStreamInfo)
