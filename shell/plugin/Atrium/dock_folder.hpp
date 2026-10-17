#pragma once
// The folders at the Dock's end, as a Mac's: the Downloads stack and the
// Trash. `DockFolder { kind: "downloads" }` lists the newest files there
// (half-done downloads left out) and says when one arrives;
// `DockFolder { kind: "trash" }` says whether the Trash is full, takes files
// into it and empties it (the FreeDesktop trash spec: $XDG_DATA_HOME/Trash
// with files/ and info/). Both watch their folder rather than poll.

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

namespace atrium {

class DockFolder : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged)
    Q_PROPERTY(QString name READ name NOTIFY kindChanged)
    Q_PROPERTY(QString path READ path NOTIFY kindChanged)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // Newest first, at most `shown`: [{name, path, url, icon, image, dir}];
    // `image` is set for pictures, shown as themselves.
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(int shown MEMBER shown_ NOTIFY changed)

public:
    explicit DockFolder(QObject* parent = nullptr);

    QString kind() const { return kind_; }
    void setKind(const QString& kind);
    QString name() const;
    QString path() const;
    int count() const { return count_; }
    QVariantList items() const { return items_; }

    // The folder in the file manager (the Trash as trash:///).
    Q_INVOKABLE void openFolder() const;
    // An item, with the app that opens its kind.
    Q_INVOKABLE void openItem(const QString& path) const;
    // Into the Trash (the trash kind only).
    Q_INVOKABLE void trash(const QList<QUrl>& urls) const;
    // Everything in the Trash, gone for good.
    Q_INVOKABLE void empty() const;

signals:
    void kindChanged();
    void changed();
    // A new file landed in it (a finished download).
    void arrived();

private:
    void rescan();
    QString listed() const;  // the folder whose entries are listed

    QString kind_;
    QFileSystemWatcher watcher_;
    QTimer settle_;  // a burst of changes is read once
    QVariantList items_;
    int count_ = 0;
    int shown_ = 12;
    QStringList known_;  // names seen last time, to tell arrivals
    bool scanned_ = false;
};

} // namespace atrium
