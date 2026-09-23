#pragma once
// The desktop folder for the desktop icons: its files (folders first, by
// name), which are selected, which is being renamed, and what the context
// menu does with them. `DesktopFiles {}`.

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QUrl>

#include <vector>

namespace atrium {

class DesktopFiles : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder CONSTANT)
    Q_PROPERTY(QStringList selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString renaming READ renaming WRITE setRenaming NOTIFY renamingChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { PathRole = Qt::UserRole + 1, NameRole, FileNameRole, IsDirRole, IsImageRole, IconRole };

    explicit DesktopFiles(QObject* parent = nullptr);

    QString folder() const { return folder_; }
    QStringList selection() const { return selection_; }
    QString renaming() const { return renaming_; }
    void setRenaming(const QString& path);
    int count() const { return int(files_.size()); }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // A click on a file ("" for the bare desktop): Ctrl toggles it in the
    // selection, a right click keeps a selection it is part of.
    Q_INVOKABLE void click(const QString& path, bool ctrl, bool right);
    // What an action on `path` applies to: the selection when it is in it.
    Q_INVOKABLE QStringList targets(const QString& path) const;

    Q_INVOKABLE void open(const QString& path);  // the targets
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void terminalHere();
    Q_INVOKABLE void newFolder();  // and starts renaming it
    Q_INVOKABLE void rename(const QString& path, const QString& name);
    Q_INVOKABLE void trash(const QString& path);  // the targets
    Q_INVOKABLE void copyPath(const QString& path);
    // Files dropped from an app move in. False if none came from elsewhere.
    Q_INVOKABLE bool moveIn(const QList<QUrl>& urls);

signals:
    void selectionChanged();
    void renamingChanged();
    void countChanged();

private:
    struct File {
        QString path, name, fileName, icon;
        bool dir = false, image = false;
        bool operator==(const File&) const = default;
    };

    void reload();
    void setSelection(QStringList selection);

    QString folder_;
    std::vector<File> files_;
    QStringList selection_;
    QString renaming_;
    QFileSystemWatcher watcher_;
    QTimer settle_;  // a burst of changes reloads once
};

} // namespace atrium
