// atrium-portal: atrium's xdg-desktop-portal backend, started by D-Bus when
// the portal first needs it. A plain (not GUI) Qt program on purpose: a GUI
// one asks the portal for its settings as it starts, while the portal is
// still waiting for this backend to start, and both hang.

#include "portal.hpp"

#include <QCoreApplication>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("atrium-portal"));
    if (!atrium::PortalBackend::instance()->start())
        return 1;  // another backend already has the name
    return app.exec();
}
