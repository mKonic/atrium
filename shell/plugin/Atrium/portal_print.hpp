#pragma once
// The Print portal: atrium's print dialog (print.qml) in place of GTK's.
// PreparePrint asks (printer, copies, pages, paper, sides, colour) and the
// app renders by the answer; Print then sends what it rendered to CUPS
// (lp), or saves it as the PDF chosen. A Print with no PreparePrint before
// it asks first.

#include "portal.hpp"

#include <QDBusAbstractAdaptor>
#include <QDBusUnixFileDescriptor>
#include <QHash>
#include <QJsonObject>
#include <QVariantMap>

namespace atrium {

class PrintAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Print")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit PrintAdaptor(PortalBackend* parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 2; }

public slots:
    uint PreparePrint(const QDBusObjectPath& handle, const QString& app, const QString& window,
                      const QString& title, const QVariantMap& settings, const QVariantMap& pageSetup,
                      const QVariantMap& options, QVariantMap& results);
    uint Print(const QDBusObjectPath& handle, const QString& app, const QString& window, const QString& title,
               const QDBusUnixFileDescriptor& fd, const QVariantMap& options, QVariantMap& results);

private:
    void ask(const QDBusObjectPath& handle, const QString& window, const QString& title, const QVariantMap& settings,
             const QVariantMap& pageSetup, const QVariantMap& options,
             std::function<QVariantList(const QJsonObject& job, QVariantMap results)> done);

    QHash<uint, QJsonObject> jobs_;  // what PreparePrint settled, by token
    uint next_ = 1;
};

// Sends a rendered document where `job` says: CUPS, or a PDF file.
bool sendPrintJob(const QJsonObject& job, const QString& title, const QByteArray& document);

} // namespace atrium
