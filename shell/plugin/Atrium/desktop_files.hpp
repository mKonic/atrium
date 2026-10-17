#pragma once
// The desktop folder for the desktop icons, as Plasma's and Finder's
// desktops treat it: where each icon sits (sorted, or where it was put),
// what is selected (clicks, Shift and Ctrl, a dragged rectangle, the
// keyboard), and what the menu, the keys and drops do with the files,
// undoable. `DesktopFiles {}`.

#include "files.hpp"

#include <QAbstractListModel>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QUrl>

#include <map>
#include <vector>

namespace atrium {

class DesktopFiles : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder CONSTANT)
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString renaming READ renaming WRITE setRenaming NOTIFY renamingChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // The grid, set by the view: the room the icons have, one cell's size,
    // and the margin around them.
    Q_PROPERTY(QSizeF area READ area WRITE setArea NOTIFY gridChanged)
    Q_PROPERTY(QSizeF cell READ cell WRITE setCell NOTIFY gridChanged)
    Q_PROPERTY(int margin READ margin WRITE setMargin NOTIFY gridChanged)
    // The rectangle being dragged out on the bare desktop; empty when none.
    Q_PROPERTY(QRectF band READ band NOTIFY bandChanged)
    // The folder or app a drag would drop into; "" for the desktop itself.
    Q_PROPERTY(QString dropTarget READ dropTarget NOTIFY dropTargetChanged)
    // What Undo undoes ("Rename"), "" when there's nothing.
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY canPasteChanged)
    // desktop.sort: manual (where they were put), name, kind, date or size.
    Q_PROPERTY(QString sortBy READ sortBy WRITE setSortBy NOTIFY sortByChanged)

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        NameRole,
        FileNameRole,
        IsDirRole,
        IsImageRole,
        IconRole,
        IsLinkRole,
        IsHiddenRole,
        SelectedRole,
        LeftRole,
        TopRole,
    };

    explicit DesktopFiles(QObject* parent = nullptr);

    QString folder() const { return folder_; }
    QStringList selection() const { return selection_; }
    QString renaming() const { return renaming_; }
    void setRenaming(const QString& path);
    int count() const { return int(files_.size()); }
    QSizeF area() const { return area_; }
    void setArea(QSizeF area);
    QSizeF cell() const { return cell_; }
    void setCell(QSizeF cell);
    int margin() const { return margin_; }
    void setMargin(int margin);
    QRectF band() const { return band_; }
    QString dropTarget() const { return dropTarget_; }
    QString undoText() const;
    bool canPaste() const;
    QString sortBy() const { return sort_; }
    void setSortBy(const QString& sort);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The mouse on a file ("" for the bare desktop), as Plasma reads it:
    // a press selects (Ctrl toggles, Shift selects the run from the last
    // one), a release without a drag narrows to the one clicked, a click on
    // the name of the only one selected renames it, a double click opens.
    Q_INVOKABLE void press(const QString& path, int modifiers, bool right);
    Q_INVOKABLE void release(const QString& path, int modifiers, bool onName);
    Q_INVOKABLE void activate(const QString& path);
    // Dragging out a rectangle on the bare desktop selects what it touches;
    // with Ctrl, toggles that in what was selected.
    Q_INVOKABLE void beginBand(qreal x, qreal y, int modifiers);
    Q_INVOKABLE void moveBand(qreal x, qreal y);
    Q_INVOKABLE void endBand();
    // A key while the desktop has the keyboard. False when it isn't ours.
    Q_INVOKABLE bool key(int key, int modifiers, const QString& text);
    // What an action on `path` applies to: the selection when it is in it.
    Q_INVOKABLE QStringList targets(const QString& path) const;
    // Where `path`'s icon is (the grid's corner for none), for a menu opened by key.
    Q_INVOKABLE QPointF spot(const QString& path) const;
    Q_INVOKABLE bool isDir(const QString& path) const { return QFileInfo(path).isDir(); }
    Q_INVOKABLE bool isLink(const QString& path) const { return QFileInfo(path).isSymLink(); }

    Q_INVOKABLE void open(const QString& path);  // the targets
    // The apps that open `path`'s kind, its default first: [{value, label, icon}].
    Q_INVOKABLE QVariantList apps(const QString& path) const;
    Q_INVOKABLE void openWith(const QString& path, const QString& appId);
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void terminalHere();
    Q_INVOKABLE void newFolder();  // and starts renaming it
    // ~/Templates' files, for New Document: [{label, path, icon}].
    Q_INVOKABLE QVariantList templates() const;
    // A copy of `templatePath`, or an empty text file for "", renamed next.
    Q_INVOKABLE void newFile(const QString& templatePath);
    Q_INVOKABLE void rename(const QString& path, const QString& name);
    Q_INVOKABLE void trash(const QString& path);  // the targets
    // For good, after the view has asked (deleteRequested).
    Q_INVOKABLE void deleteNow(const QStringList& paths);
    Q_INVOKABLE void copy(const QString& path);  // the targets, to the clipboard
    Q_INVOKABLE void cut(const QString& path);
    // The clipboard's files into the desktop, or into the folder `into`.
    Q_INVOKABLE void paste(const QString& into = {});
    Q_INVOKABLE void undo();
    Q_INVOKABLE void copyPath(const QString& path);
    // A link's target, shown in the file manager.
    Q_INVOKABLE void showOriginal(const QString& path);
    Q_INVOKABLE void reload();

    // Dragging icons: what leaves (the targets of the one grabbed), and
    // where a drop lands: on a folder it goes in, on an app it opens there,
    // and our own icons dropped on the desktop move to that spot.
    Q_INVOKABLE QString dragUris(const QString& path);
    Q_INVOKABLE void dragOver(qreal x, qreal y, const QList<QUrl>& urls);
    Q_INVOKABLE bool drop(const QList<QUrl>& urls, qreal x, qreal y, bool copy);

signals:
    void selectionChanged();
    void renamingChanged();
    void countChanged();
    void gridChanged();
    void bandChanged();
    void dropTargetChanged();
    void undoChanged();
    void canPasteChanged();
    void sortByChanged();
    // The menu key: the menu for `path` ("" for the desktop's).
    void menuRequested(const QString& path);
    // Shift+Delete: the view asks before deleteNow.
    void deleteRequested(const QStringList& paths);

private:
    struct File {
        QString path, name, fileName, icon;
        bool dir = false, image = false, link = false, hidden = false, launcher = false;
        qint64 size = 0, modified = 0;
        QString kind;  // its MIME type, for sorting by kind
        files::Cell cell;
        bool operator==(const File&) const = default;
    };
    // One undoable change: files moved (from, to), trashed (where from,
    // where in the trash), made.
    struct Step {
        QString text;
        std::vector<std::pair<QString, QString>> moved, trashed;
        QStringList made;
    };

    void layout(std::vector<File>& files) const;
    void apply(std::vector<File> next);
    void setSelection(QStringList selection);
    void selectOnly(const QString& path);
    void setCurrent(const QString& path);
    int indexOf(const QString& path) const;
    int at(qreal x, qreal y) const;  // the icon under a point, -1 for none
    files::Cell cellAt(qreal x, qreal y) const;
    int rows() const;
    int cols() const;
    std::vector<int> gridOrder() const;
    void selectRange(const QString& to);
    void step(int dx, int dy, int modifiers);
    void startRename(const QString& path);
    // Files into `dir`, moved or copied, under free names; the new paths.
    QStringList transfer(const QList<QUrl>& urls, const QString& dir, bool copy, const QString& text);
    void pushUndo(Step s);
    void loadPositions();
    void savePositions();
    void fixPositions();  // the cells shown, kept: sorting turned to manual

    QString folder_;
    std::vector<File> files_;
    QStringList selection_;
    QString current_, anchor_;  // the keyboard's file; where Shift's run starts
    QString renaming_;
    QSizeF area_, cell_{120, 108};
    int margin_ = 18;
    QRectF band_;
    QPointF bandStart_;
    QStringList pinned_;  // selected when a Ctrl band started
    QString dropTarget_;
    QString dragAnchor_;  // the icon grabbed in a drag of ours
    QString sort_ = "manual";
    bool hidden_ = false;
    std::map<QString, files::Cell> positions_;  // by file name, sorting manual
    std::map<QString, files::Cell> written_;    // as the file has them
    std::map<QString, files::Cell> landing_;    // where files on their way in go
    std::vector<Step> undo_;
    bool soleWhenPressed_ = false;
    QTimer renameLater_;  // a click on the name renames, unless a second click opens
    QString renameCandidate_;
    QString typed_;
    QTimer typedReset_;
    QFileSystemWatcher watcher_;
    QTimer settle_;  // a burst of changes reloads once
};

} // namespace atrium
