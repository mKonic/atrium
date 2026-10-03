#pragma once
// A folder for a file picker: its files (folders first, natural order), the
// ones a name filter lets through, kept up to date as the folder changes.
// `FolderModel { folder: "/home/me/Pictures"; filter: "Pictures (*.png)" }`.
// The file chooser filters by `patterns` instead ([{ glob } or { mime }],
// "image/*" too), and picks several with `select`.

#include <QAbstractListModel>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVariantList>

#include <string>
#include <vector>

namespace atrium {

class FolderModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY folderChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QVariantList patterns READ patterns WRITE setPatterns NOTIFY filterChanged)
    Q_PROPERTY(bool foldersOnly READ foldersOnly WRITE setFoldersOnly NOTIFY filterChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    // The paths picked, in the folder's order; several only when `multiple`.
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(bool multiple READ multiple WRITE setMultiple NOTIFY selectionChanged)
    // The folder and those above it, for a path bar: [{ name, path }], root first.
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY folderChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Places to jump to: home, its usual folders, mounted drives. [{ name, icon, path }]
    Q_PROPERTY(QVariantList places READ places CONSTANT)

public:
    enum Role { PathRole = Qt::UserRole + 1, NameRole, IsDirRole, IsImageRole, IconRole, SizeRole, ModifiedRole, SelectedRole };
    // How a click picks: alone, added or taken away (Ctrl), or everything
    // from the last one picked (Shift).
    enum Pick { Only, Toggle, Extend };
    Q_ENUM(Pick)

    explicit FolderModel(QObject* parent = nullptr);

    QString folder() const { return folder_; }
    void setFolder(const QString& path);
    QString filter() const { return filter_; }
    void setFilter(const QString& filter);
    QVariantList patterns() const { return patterns_; }
    void setPatterns(const QVariantList& patterns);
    bool foldersOnly() const { return foldersOnly_; }
    void setFoldersOnly(bool on);
    bool showHidden() const { return showHidden_; }
    void setShowHidden(bool on);
    QStringList selection() const;
    bool multiple() const { return multiple_; }
    void setMultiple(bool on);
    QVariantList crumbs() const;
    int count() const { return int(files_.size()); }
    QVariantList places() const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void up();
    // What a typed path means: { path, exists, isDir, allowed } (allowed:
    // a file the filter lets through).
    Q_INVOKABLE QVariantMap resolve(const QString& typed) const;
    Q_INVOKABLE int indexOf(const QString& path) const;
    Q_INVOKABLE void select(int index, atrium::FolderModel::Pick how = Only);
    Q_INVOKABLE void clearSelection();

signals:
    void folderChanged();
    void filterChanged();
    void showHiddenChanged();
    void countChanged();
    void selectionChanged();

private:
    struct File {
        QString path, name, icon, size, modified;
        bool isDir = false, isImage = false, selected = false;
    };

    void reload();
    bool lets(const QFileInfo& file) const;  // through the filter
    void setSelected(size_t i, bool on);

    QString folder_;
    QString filter_;
    bool showHidden_ = false;
    bool foldersOnly_ = false;
    bool multiple_ = false;
    std::vector<std::string> suffixes_;
    QVariantList patterns_;
    std::vector<std::string> globs_;
    QStringList mimes_;
    int anchor_ = -1;  // where Shift picks from
    std::vector<File> files_;
    QFileSystemWatcher watcher_;
    QTimer settle_;  // a burst of changes, one reload
};

} // namespace atrium
