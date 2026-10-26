#pragma once
// The file chooser's list, as GTK 3's GtkFileChooserWidget has it: a folder
// (browse), the recently used files (recent), or what a search finds under
// a folder (search). Columns Name, Location (recent and search), Size,
// Type, Modified; sortable, folders first if wanted; choosing a folder
// shows files greyed. Rules in chooser_core.

#include "chooser_core.hpp"

#include <QAbstractListModel>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace atrium {

class ChooserModel : public QAbstractListModel {
    Q_OBJECT
    // "open", "save" or "folder" (choosing a folder).
    Q_PROPERTY(QString action READ action WRITE setAction NOTIFY actionChanged)
    // "browse", "recent" or "search".
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // [{ glob } | { mime }]; empty lets everything through.
    Q_PROPERTY(QVariantList patterns READ patterns WRITE setPatterns NOTIFY patternsChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY viewChanged)
    Q_PROPERTY(bool foldersFirst READ foldersFirst WRITE setFoldersFirst NOTIFY viewChanged)
    // "name", "size", "type" or "modified".
    Q_PROPERTY(QString sortColumn READ sortColumn WRITE setSortColumn NOTIFY viewChanged)
    Q_PROPERTY(bool ascending READ ascending WRITE setAscending NOTIFY viewChanged)
    Q_PROPERTY(bool showTime READ showTime WRITE setShowTime NOTIFY viewChanged)
    Q_PROPERTY(bool clock24 READ clock24 WRITE setClock24 NOTIFY viewChanged)
    Q_PROPERTY(bool multiple READ multiple WRITE setMultiple NOTIFY selectionChanged)
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(bool allFolders READ allFolders NOTIFY selectionChanged)
    Q_PROPERTY(bool allFiles READ allFiles NOTIFY selectionChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    // Why the folder can't be shown ("" when it can).
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // The path bar: [{ name, path, kind: root|home|desktop|folder, current, hidden }],
    // root first. Going up keeps the deeper ones (a click goes back down).
    Q_PROPERTY(QVariantList trail READ trail NOTIFY trailChanged)
    Q_PROPERTY(QString home READ home CONSTANT)

public:
    enum Role {
        PathRole = Qt::UserRole + 1, NameRole, IsDirRole, SensitiveRole, IconRole, ThumbRole, SizeTextRole,
        TypeRole, DateTextRole, TimeTextRole, LocationRole, SelectedRole, HiddenRole,
    };
    enum Pick { Only, Toggle, Extend };
    Q_ENUM(Pick)

    explicit ChooserModel(QObject* parent = nullptr);
    ~ChooserModel() override;

    QString action() const { return action_; }
    void setAction(const QString& action);
    QString mode() const { return mode_; }
    QString folder() const { return folder_; }
    void setFolder(const QString& path);
    QString query() const { return query_; }
    void setQuery(const QString& query);
    QVariantList patterns() const { return patterns_; }
    void setPatterns(const QVariantList& patterns);
    bool showHidden() const { return showHidden_; }
    void setShowHidden(bool on);
    bool foldersFirst() const { return foldersFirst_; }
    void setFoldersFirst(bool on);
    QString sortColumn() const;
    void setSortColumn(const QString& column);
    bool ascending() const { return ascending_; }
    void setAscending(bool on);
    bool showTime() const { return showTime_; }
    void setShowTime(bool on);
    bool clock24() const { return clock24_; }
    void setClock24(bool on);
    bool multiple() const { return multiple_; }
    void setMultiple(bool on);
    QStringList selection() const;
    bool allFolders() const;
    bool allFiles() const;
    int count() const { return int(rows_.size()); }
    bool searching() const { return searching_; }
    QString error() const { return error_; }
    QVariantList trail() const;
    QString home() const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Shows the recently used files (choosing a folder or saving: their folders).
    Q_INVOKABLE void showRecent();
    // Searching starts with a query, in the folder being browsed.
    Q_INVOKABLE void startSearch();
    Q_INVOKABLE void stopSearch();

    Q_INVOKABLE void select(int index, atrium::ChooserModel::Pick how = Only);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    // Picks `path` once its folder is shown (going there first); a hidden
    // one turns hidden files on, one the filter hides turns the filter off
    // (asks through filterInTheWay).
    Q_INVOKABLE void selectPath(const QString& path);
    Q_INVOKABLE int indexOf(const QString& path) const;
    Q_INVOKABLE QVariantMap row(int index) const;
    // A double click or Return on a row: { enter: path } or { answer: true } or {}.
    Q_INVOKABLE QVariantMap activate(int index);
    // A path bar button: keeps the trail, and picks the folder it came from.
    Q_INVOKABLE void goTrail(const QString& path);
    Q_INVOKABLE void up();    // Alt+Up
    Q_INVOKABLE void down();  // Alt+Down: back along the trail
    Q_INVOKABLE void reload();

    // For the location field: the names in the folder `text` points into
    // that start with what follows its last "/" (folders end in "/"), each
    // with the text before it; and Tab's common start.
    Q_INVOKABLE QVariantList completions(const QString& text) const;
    Q_INVOKABLE QString complete(const QString& text) const;
    // What `text` means against the folder shown: { path, exists, isDir, dir, file }.
    Q_INVOKABLE QVariantMap resolve(const QString& text) const;

signals:
    void actionChanged();
    void modeChanged();
    void folderChanged();
    void queryChanged();
    void patternsChanged();
    void viewChanged();
    void selectionChanged();
    void countChanged();
    void searchingChanged();
    void errorChanged();
    void trailChanged();
    // The cursor row a newly shown folder or a picked path wants in view.
    void reveal(int index);
    // A file asked for is filtered out: the dialog drops the filter.
    void filterInTheWay();

private:
    struct Row {
        QString path, name, icon, thumb, sizeText, type, location;
        bool isDir = false, hidden = false, selected = false, sensitive = true;
        qint64 size = 0, time = 0;
        std::string nameKey;
        std::string dirKey;  // recent and search: the folder, for sorting by where
    };

    // What decides a row, copied so the search thread needn't touch the model.
    struct Lens {
        chooser::Action act = chooser::Action::Open;
        bool located = false;  // recent and search: rows say where they are
        bool filtered = false;
        std::vector<std::string> globs;
        QStringList mimes;
    };
    Lens lens() const;
    static Row buildRow(const Lens& lens, const QFileInfo& info, qint64 time);
    static bool passes(const Lens& lens, const QFileInfo& info);
    Row makeRow(const QFileInfo& info, qint64 time) const { return buildRow(lens(), info, time); }
    bool lets(const QFileInfo& info) const { return passes(lens(), info); }
    // Search hits arrive in order: added at the end, nothing else redone.
    void appendRows(std::vector<Row> rows);
    void setRows(std::vector<Row> rows);
    void sortRows(std::vector<Row>& rows) const;
    void loadFolder();
    void loadRecent();
    void runSearch();
    void cancelSearch();
    void setMode(const QString& mode);
    void setError(const QString& error);
    void setTrail(const QString& path, bool keep);
    void applyPending();
    chooser::Action act() const;

    QString action_ = "open", mode_ = "browse", folder_, query_, error_;
    QVariantList patterns_;
    std::vector<std::string> globs_;
    QStringList mimes_;
    bool showHidden_ = false, foldersFirst_ = true, ascending_ = true, showTime_ = false, clock24_ = true;
    bool multiple_ = false, searching_ = false;
    chooser::SortColumn sort_ = chooser::SortColumn::Name;
    std::vector<Row> rows_;
    int anchor_ = -1;
    QStringList pending_;    // to pick once shown
    QStringList trail_;      // the path bar's folders, deepest last
    QFileSystemWatcher watcher_;
    QTimer settle_;
    QTimer queryDelay_;  // typing settles before a search starts (GtkSearchEntry's 150 ms)
    std::shared_ptr<std::atomic<bool>> searchStop_;
    std::thread searchThread_;
};

} // namespace atrium
