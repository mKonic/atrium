// atrium-clipsync: shares the clipboard with a phone over Bluetooth (see
// sync.hpp). atrium starts it with the session; it idles while
// bluetooth.phone_clipboard is off.

#include "sync.hpp"

#include <QCoreApplication>
#include <QDBusConnection>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("atrium-clipsync"));
    // One per session: a second (a nested atrium's) would fight over BlueZ.
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("org.atrium.ClipSync")))
        return 0;
    atrium::clipsync::Sync sync;
    if (!sync.ok())
        return 1;
    return app.exec();
}
