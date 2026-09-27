#pragma once
// A folder for a file picker: its files (folders first, natural order), the
// ones a name filter lets through, kept up to date as the folder changes.
// `FolderModel { folder: "/home/me/Pictures"; filter: "Pictures (*.png)" }`.

#include <QAbstractListModel>
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
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    // The folder and those above it, for a path bar: [{ name, path }], root first.
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY folderChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Places to jump to: home, its usual folders, mounted drives. [{ name, icon, path }]
    Q_PROPERTY(QVariantList places READ places CONSTANT)

public:
    enum Role { PathRole = Qt::UserRole + 1, NameRole, IsDirRole, IsImageRole, IconRole, SizeRole, ModifiedRole };

    explicit FolderModel(QObject* parent = nullptr);

    QString folder() const { return folder_; }
    void setFolder(const QString& path);
    QString filter() const { return filter_; }
    void setFilter(const QString& filter);
    bool showHidden() const { return showHidden_; }
    void setShowHidden(bool on);
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

signals:
    void folderChanged();
    void filterChanged();
    void showHiddenChanged();
    void countChanged();

private:
    struct File {
        QString path, name, icon, size, modified;
        bool isDir = false, isImage = false;
    };

    void reload();

    QString folder_;
    QString filter_;
    bool showHidden_ = false;
    std::vector<std::string> suffixes_;
    std::vector<File> files_;
    QFileSystemWatcher watcher_;
    QTimer settle_;  // a burst of changes, one reload
};

} // namespace atrium
