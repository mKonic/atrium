#pragma once
// The palette's root search (Super+Space), after Tinycast's launcher: every
// entry kind (apps, Settings pages, quicklinks, snippets, system actions,
// window commands, custom commands, the palette's own commands, open windows)
// ranked by rank.hpp, learned from what you open; with nothing typed,
// Favorites, Suggestions and a section per kind. Each row has an actions
// menu (Ctrl+K), favorites have Ctrl+1..0, an alias or a hide lives in the
// launcher_entries registry table, and a query nothing answers is offered to
// the fallbacks ("Use “…” with").
//
//   LauncherModel { entries: DesktopEntries; query: field.text }

#include "apps.hpp"
#include "launcher_ranking.hpp"

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <memory>
#include <vector>

namespace atrium {

class SettingsPages;

class LauncherModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QObject* entries READ entries WRITE setEntries NOTIFY entriesChanged)
    // What the search field holds: the root search's query, or the filter of
    // the screen on top (Clipboard, Emoji, ...), which binds to it.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // The screen showing: root, clipboard, emoji, files, windows, snippets,
    // quicklinks, calculator, output. Screens opened from the palette stack
    // over it; a back step returns with the query and row as they were.
    Q_PROPERTY(QString screen READ screen NOTIFY screenChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY screenChanged)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // The selected row's arguments, asked for beside the search field:
    // [{ name, optional }]. Empty for most rows.
    Q_PROPERTY(QVariantList arguments READ arguments NOTIFY currentChanged)
    // The custom command whose output the output screen shows.
    Q_PROPERTY(QString outputTitle READ outputTitle NOTIFY outputChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(bool outputRunning READ outputRunning NOTIFY outputChanged)
    Q_PROPERTY(int outputExit READ outputExit NOTIFY outputChanged)

public:
    enum Role {
        KindRole = Qt::UserRole + 1,  // app, settings, quicklink, ..., calc, url, fallback
        KeyRole,       // the entry's key ("app:firefox.desktop"); "" for rows of this query only
        SectionRole,   // the header it sits under ("" none)
        TitleRole,
        DetailRole,    // dim text after the title
        IconRole,      // a theme icon name or file URL; "" draws the glyph
        GlyphRole,     // Material Symbols
        ColorRole,     // the glyph's square (Settings pages); "" none
        LabelRole,     // what it is, at the right ("Application")
        AliasRole,
        KeysRole,      // its global shortcut ("Mod+Shift+T"), "" none
        SlotRole,      // favorite number for Ctrl+N (1..10), 0 none
        RunningRole,   // an app with windows open
        BadgeRole,     // a calculation's input badge ("Kilometres"); label is its answer's
    };

    explicit LauncherModel(QObject* parent = nullptr);
    ~LauncherModel() override;

    QObject* entries() const { return index_.source(); }
    void setEntries(QObject* entries);
    QString query() const { return query_; }
    void setQuery(const QString& query);
    int current() const { return current_; }
    void setCurrent(int current);
    int count() const { return int(rows_.size()); }
    QString screen() const { return screen_; }
    bool canGoBack() const { return !stack_.empty(); }
    QVariantList arguments() const;
    QString outputTitle() const { return outputTitle_; }
    QString output() const { return output_; }
    bool outputRunning() const { return process_ && process_->state() != QProcess::NotRunning; }
    int outputExit() const { return outputExit_; }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The palette opens on `screen` (a root: nothing to step back to),
    // sources re-read, the field holding `query`.
    Q_INVOKABLE void open(const QString& screen = QStringLiteral("root"), const QString& query = {});
    // Open a screen over the current one.
    Q_INVOKABLE void push(const QString& screen, const QString& query = {});
    // Escape: a query is cleared first, then a pushed screen steps back.
    // False when there is nothing left to do but close.
    Q_INVOKABLE bool escape();
    // Backspace in an empty field: steps back, never closes. False: nothing to step back to.
    Q_INVOKABLE bool backspace();
    // Tab: the root search and Clipboard History, carrying the query across.
    Q_INVOKABLE void tab();
    Q_INVOKABLE void move(int delta);
    // Do what the row offers (the selected one by default), with the
    // arguments typed beside the search field.
    Q_INVOKABLE void activate(int row = -1, const QVariantList& args = {});
    // The row's actions menu: [{ id, title, glyph, keys, group }], the
    // first being what Enter does; `group` starts a new block. With a
    // query, only the ones it finds.
    Q_INVOKABLE QVariantList actions(int row, const QString& query = {}) const;
    Q_INVOKABLE void runAction(int row, const QString& id);
    // A chord on the selected row (favorite, hide, favorite-up, favorite-down,
    // quit, restart, reveal): false when that row offers no such thing.
    Q_INVOKABLE bool chord(const QString& id);
    // Ctrl+1..9, Ctrl+0 (10): open that favorite.
    Q_INVOKABLE bool openFavorite(int slot);
    // A global shortcut's "run:<key>": that entry, the palette unopened.
    Q_INVOKABLE void run(const QString& key);
    // Answer to confirmRequested.
    Q_INVOKABLE void confirm(const QString& token);
    // For Settings: every entry that can carry an alias, a shortcut or a
    // hide, by kind then name, matching `query`: [{ key, title, section,
    // label, icon, glyph, color, alias, hidden, hideable, keys, shortcut }]
    // (`shortcut` is the id of its shortcut record, -1 none).
    Q_INVOKABLE QVariantList items(const QString& query = {});
    Q_INVOKABLE void setAlias(const QString& key, const QString& alias);
    Q_INVOKABLE void setHidden(const QString& key, bool hidden);
    // Window layouts, for Settings: [{ name, windows }].
    Q_INVOKABLE QVariantList windowLayouts() const;
    Q_INVOKABLE void renameLayout(const QString& from, const QString& to);
    Q_INVOKABLE void removeLayout(const QString& name);
    // A snippet's keyword typed in an app: it becomes the snippet.
    Q_INVOKABLE void expandSnippet(qint64 snippet, int before);
    Q_INVOKABLE void stopOutput();
    Q_INVOKABLE void rerunOutput();
    Q_INVOKABLE void copyOutput() const;

signals:
    void entriesChanged();
    void queryChanged();
    void currentChanged();
    void countChanged();
    void screenChanged();
    // Open the palette on the root search with this in the field (a global
    // shortcut to something that asks for arguments).
    void openRequested(const QString& query);
    void outputChanged();
    void closeRequested();
    // A pill saying what an action did out of sight ("Trash Emptied").
    void feedback(const QString& glyph, const QString& text, bool noop);
    // Ask before running: confirm(token) runs it.
    void confirmRequested(const QString& title, const QString& detail, const QString& glyph, const QString& token);

private:
    enum class Kind { App, Settings, Quicklink, Snippet, System, Window, Script, Command, OpenWindow };
    struct Entry {
        QString key;
        Kind kind;
        QString title, detail, icon, glyph, color;
        QString target;  // desktop id, page, record id, catalog id, window id
        bool confirm = false;
        bool inRoot = true;  // a quicklink kept out of root search is still in Search Quicklinks
        rank::Fields fields;
    };
    struct Row {
        QString kind;          // Kind's id, or calc / url / fallback / run
        int entry = -1;        // index into entries_
        QString section;
        QString title, detail, icon, glyph, label;
        QString target;        // what a synthetic row acts on
        int slot = 0;
        QString badge;
    };
    struct Prefs {
        QString alias;
        std::optional<int> favorite;
        bool hidden = false;
    };

    static const char* kindId(Kind k);
    const Prefs* prefs(const QString& key) const;
    QString keysFor(const QString& key) const;
    bool running(const Entry& e) const;
    bool persistent(const Entry& e) const { return e.kind != Kind::OpenWindow; }
    // The screens this model lists: the root search, and one kind of it
    // (windows, snippets, quicklinks).
    bool listScreen() const;
    std::optional<Kind> screenKind() const;

    QVariantList allActions(int row) const;
    struct Uninstall {
        QString command;    // what removes it ("" can't tell)
        QString says;       // what the confirmation explains
        QString trashFile;  // a launcher entry of the user's own, trashed instead
    };
    Uninstall uninstallFor(const Entry& e) const;
    void rebuildEntries();
    void rebuildEntriesLater();
    void rebuildRows(bool keepSelection = false);
    void pushEntryRow(std::vector<Row>& rows, int entry, const QString& section, int slot = 0) const;
    void runEntry(const Entry& e, const QVariantList& args, bool fromPalette);
    void runRow(const Row& r, const QVariantList& args);
    void runCommand(const QString& id);
    void runCustom(const QVariantMap& record, const QVariantList& args);
    void openQuicklink(const QVariantMap& record, const QVariantList& args);
    void typeSnippet(const QVariantMap& record, const QVariantList& args, bool copyOnly);
    QString expand(const QString& text, const QVariantList& args, int encoding) const;
    QVariantMap record(const QString& table, const QString& id) const;
    void setPref(const QString& key, const QVariantMap& fields);
    void toggleFavorite(const Entry& e);
    void moveFavorite(const Entry& e, int direction);
    void quitApp(const Entry& e, bool restart);
    const Entry* entryAt(int row) const;
    int rowForKey(const QString& key) const;

    EntryIndex index_;
    std::unique_ptr<SettingsPages> pages_;
    RankingStore ranking_;
    std::vector<Entry> entries_;
    QHash<QString, int> byKey_;
    QHash<QString, Prefs> prefs_;
    QHash<QString, QString> keys_;  // entry key → its global shortcut
    QSet<QString> runningApps_;     // desktop ids with windows
    std::vector<Row> rows_;
    QString query_;
    int current_ = 0;
    QString selectedKey_;
    struct Frame {
        QString screen, query;
        int current;
    };
    QString screen_ = QStringLiteral("root");
    std::vector<Frame> stack_;
    QTimer rebuildTimer_;

    // Calculator History: what was copied from a calculation, newest first
    // ({ input, result, copy, badge, label }), kept in the state directory.
    QVariantList calcHistory_;
    void loadCalcHistory();
    void rememberCalc(const Row& r);
    void saveCalcHistory() const;

    // The custom command whose output shows.
    std::unique_ptr<QProcess> process_;
    QString outputTitle_, output_;
    int outputExit_ = 0;
    QVariantMap outputRecord_;
    QVariantList outputArgs_;
};

} // namespace atrium
