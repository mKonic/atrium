#pragma once
// Window layouts, after Tinycast's: a saved arrangement (these apps, on
// these screens, at these places as shares of the screen) that the palette
// puts back in one go: windows already open move into place, apps that
// aren't running start and their windows go where they belong as they
// appear. Stored as rows of the layout_windows registry table; a layout is
// its name.

#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariant>

namespace atrium {

class EntryIndex;

class WindowLayouts : public QObject {
    Q_OBJECT

public:
    static WindowLayouts* instance();

    // Every layout: [{ name, windows }] in the order they were saved.
    QVariantList layouts() const;
    // The focused screen's current space saved as a new layout; its name
    // ("Layout 2"), or "" when there's nothing to save.
    QString capture();
    // Put the named layout back. How many windows it placed now, and how
    // many it is still waiting for (apps starting).
    void run(const QString& name, const EntryIndex& apps);
    void rename(const QString& from, const QString& to);
    void remove(const QString& name);

signals:
    void placed(const QString& layout, int count);

private:
    WindowLayouts();
    struct Pending {
        QVariantMap row;
        qint64 until;
    };
    void place(int window, const QVariantMap& row, const QVariantMap& window_info) const;
    void windowsChanged();

    QList<Pending> pending_;
    QSet<int> known_;  // windows already there when the layout ran
    QString running_;
};

} // namespace atrium
