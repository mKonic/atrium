#pragma once
// The Settings app's table of contents: the compositor's settings grouped
// into pages in a sensible order, each with its icon and colour as System
// Settings has them, and search across all of them. `SettingsPages`.

#include <QObject>
#include <QVariant>

namespace atrium {

class SettingsPages : public QObject {
    Q_OBJECT
    // [{ name, icon, color, special }] in sidebar order; special pages
    // ("About") have their own view rather than generated rows.
    Q_PROPERTY(QVariantList pages READ pages NOTIFY changed)
    // page name → its settings, in schema order
    Q_PROPERTY(QVariantMap byPage READ byPage NOTIFY changed)

public:
    explicit SettingsPages(QObject* parent = nullptr);

    QVariantList pages() const { return pages_; }
    QVariantMap byPage() const { return byPage_; }

    // Settings whose title, description or page match `query`, best first.
    Q_INVOKABLE QVariantList search(const QString& query) const;

signals:
    void changed();

private:
    void rebuild();

    QVariantList pages_;
    QVariantMap byPage_;
};

} // namespace atrium
