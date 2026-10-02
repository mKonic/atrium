// atrium-phonelink: plays a paired phone's audio, sent over the network (see
// daemon.hpp). atrium starts it with the session; it idles while phone.audio
// is off. The phone's side is the atrium-clipsync-ksu module.

#include "daemon.hpp"

#include <QCoreApplication>
#include <QDBusConnection>

#include <pipewire/pipewire.h>

int main(int argc, char** argv) {
    pw_init(&argc, &argv);
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("atrium-phonelink"));
    // One per session: two would both take the phone's sound.
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("org.atrium.PhoneLink")))
        return 0;
    atrium::phonelink::Daemon daemon;
    const int r = app.exec();
    pw_deinit();
    return r;
}
