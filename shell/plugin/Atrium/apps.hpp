#pragma once
// Apps for the launcher and the Dock. Quickshell reads the desktop entries
// (and launches them); QML hands its DesktopEntries singleton in as
// `entries`, and everything else happens here.

#include <QAbstractListModel>
#include <QTimer>
#include <QPointer>
#include <QVariant>

#include <vector>

namespace atrium {

// Desktop entry lookups on Quickshell's DesktopEntries singleton.
class EntryIndex {
public:
    void setSource(QObject* entries) { entries_ = entries; }
    QObject* source() const { return entries_; }
    QObject* byId(const QString& id) const;
    // The entry a window's app id belongs to, or null.
    QObject* forApp(const QString& appId) const;
    // Entry id for a window's app id, so windows and pins meet.
    QString idForApp(const QString& appId) const;
    QString nameForApp(const QString& appId) const;
    QList<QObject*> all() const;

private:
    QObject* call(const char* method, const QString& arg) const;
    QPointer<QObject> entries_;
};

// Spotlight results: `LauncherResults { entries: DesktopEntries; query: input.text }`.
// Apps, open windows, sums and ">" commands, best first.
class LauncherResults : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QObject* entries READ entries WRITE setEntries NOTIFY entriesChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Only apps, no windows, sums or commands (the Settings app's picker).
    Q_PROPERTY(bool appsOnly MEMBER appsOnly_ NOTIFY queryChanged)

public:
    enum Role { KindRole = Qt::UserRole + 1, TitleRole, SubtitleRole, IconRole, GlyphRole, AppIdRole };

    explicit LauncherResults(QObject* parent = nullptr);

    QObject* entries() const { return index_.source(); }
    void setEntries(QObject* entries);
    QString query() const { return query_; }
    void setQuery(const QString& query);
    int current() const { return current_; }
    void setCurrent(int current);
    int count() const { return int(rows_.size()); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Step the selection, wrapping.
    Q_INVOKABLE void move(int delta);
    // Do what a row offers (the selected one by default). False if nothing to do.
    Q_INVOKABLE bool activate(int row = -1);

signals:
    void entriesChanged();
    void queryChanged();
    void currentChanged();
    void countChanged();

private slots:
    void rebuildLater();

private:
    struct Row {
        QString kind;  // app, window, calc, run
        QString title, subtitle, icon, glyph;
        QPointer<QObject> entry;
        int window = 0;
        QString text;  // what calc copies or run runs
    };

    void rebuild();
    void loadCounts();
    void remember(const QString& id);

    EntryIndex index_;
    QString query_;
    int current_ = 0;
    std::vector<Row> rows_;
    QVariantMap counts_;  // entry id → launches
    QString countsFile_;
    bool appsOnly_ = false;
};

// The Dock's apps: pinned ones, then running ones that aren't:
// `DockApps { entries: DesktopEntries }`.
class DockApps : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QObject* entries READ entries WRITE setEntries NOTIFY entriesChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { AppIdRole = Qt::UserRole + 1, NameRole, IconRole, PinnedRole, RunningRole, FocusedRole,
                WindowCountRole, DividerRole, LeavingRole };

    explicit DockApps(QObject* parent = nullptr);

    QObject* entries() const { return index_.source(); }
    void setEntries(QObject* entries);
    int count() const { return int(apps_.size()); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Launch it, or focus it, stepping through its windows when it is in front.
    // True if it was launched.
    Q_INVOKABLE bool activate(const QString& appId);
    Q_INVOKABLE void launch(const QString& appId);
    Q_INVOKABLE void setPinned(const QString& appId, bool pinned);
    // Pin it (if it isn't) at `index` among the pinned apps.
    Q_INVOKABLE void placePin(const QString& appId, int index);
    Q_INVOKABLE int pinnedCount() const;
    Q_INVOKABLE void closeAll(const QString& appId);
    // App exposé: the overview with only this app's windows.
    Q_INVOKABLE void expose(const QString& appId);

signals:
    void entriesChanged();
    void countChanged();

private slots:
    void rebuildLater();

private:
    struct App {
        QString id, name, icon;
        bool pinned = false;
        bool focused = false;
        bool divider = false;  // first running-only app after the pinned ones
        bool leaving = false;  // closed: still here a moment, to shrink away
        QList<int> windows;    // most recently used first
        bool operator==(const App&) const = default;
    };

    void rebuild();
    const App* find(const QString& id) const;

    EntryIndex index_;
    std::vector<App> apps_;
    // Apps that just closed stay a moment (leaving) so the Dock can shrink
    // them away instead of snapping shut; `sweep_` drops them after.
    static constexpr int kLeaveMs = 260;
    QHash<QString, qint64> leavingSince_;
    QTimer sweep_;
};

} // namespace atrium
