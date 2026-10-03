// atrium-phonelink: plays a paired phone's audio, sent over the network (see
// daemon.hpp). atrium starts it with the session; it idles while phone.audio
// is off. The phone's side is the atrium-clipsync-ksu module.

#include "daemon.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QSocketNotifier>

#include <pipewire/pipewire.h>
#include <wayland-client.h>

int main(int argc, char** argv) {
    pw_init(&argc, &argv);
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("atrium-phonelink"));
    // One per session: two would both take the phone's sound.
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("org.atrium.PhoneLink")))
        return 0;
    // The session ends with the compositor: when its Wayland connection
    // goes, so does this (and the phone gets its sound back). Nothing is
    // asked of the connection but that it stays up.
    if (wl_display* display = wl_display_connect(nullptr)) {
        auto* n = new QSocketNotifier(wl_display_get_fd(display), QSocketNotifier::Read, &app);
        QObject::connect(n, &QSocketNotifier::activated, &app, [display, n] {
            if (wl_display_dispatch(display) < 0) {
                n->setEnabled(false);
                QCoreApplication::exit(0);
            }
        });
        wl_display_flush(display);
    }
    atrium::phonelink::Daemon daemon;
    const int r = app.exec();
    pw_deinit();
    return r;
}
