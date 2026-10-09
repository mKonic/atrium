#pragma once
// The FileChooser portal: open and save dialogs for apps (sandboxed ones,
// and GTK and Electron apps that ask the portal), atrium's own chooser
// (files.qml) in place of GTK's.

#include "portal.hpp"

#include <QDBusAbstractAdaptor>
#include <QStringList>
#include <QVariantMap>

namespace atrium {

// A file filter as the portal passes them: (sa(us)), its name and rules,
// each a glob (0) or a MIME type (1).
struct PortalFilterRule {
    uint type = 0;
    QString pattern;
    bool operator==(const PortalFilterRule&) const = default;
};
struct PortalFilter {
    QString name;
    QList<PortalFilterRule> rules;
    bool operator==(const PortalFilter&) const = default;
};
using PortalFilters = QList<PortalFilter>;

QDBusArgument& operator<<(QDBusArgument& arg, const PortalFilterRule& r);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalFilterRule& r);
QDBusArgument& operator<<(QDBusArgument& arg, const PortalFilter& f);
const QDBusArgument& operator>>(const QDBusArgument& arg, PortalFilter& f);

class FileChooserAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")
    Q_PROPERTY(uint version READ version CONSTANT)

public:
    explicit FileChooserAdaptor(PortalBackend* parent);
    uint version() const { return 4; }

public slots:
    uint OpenFile(const QDBusObjectPath& handle, const QString& app, const QString& window, const QString& title,
                  const QVariantMap& options, QVariantMap& results);
    uint SaveFile(const QDBusObjectPath& handle, const QString& app, const QString& window, const QString& title,
                  const QVariantMap& options, QVariantMap& results);
    uint SaveFiles(const QDBusObjectPath& handle, const QString& app, const QString& window, const QString& title,
                   const QVariantMap& options, QVariantMap& results);

private:
    void ask(const QString& mode, const QDBusObjectPath& handle, const QString& app, const QString& title,
             const QVariantMap& options, const QString& window);
};

} // namespace atrium

Q_DECLARE_METATYPE(atrium::PortalFilterRule)
Q_DECLARE_METATYPE(atrium::PortalFilter)
Q_DECLARE_METATYPE(atrium::PortalFilters)
