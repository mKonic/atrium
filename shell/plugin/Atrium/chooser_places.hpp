#pragma once
// The file chooser's sidebar, as GTK 3's GtkPlacesSidebar fills it for a
// file chooser (local only, Other Locations on): Recent, Home, Desktop,
// the XDG folders, removable drives (mounted or not, with eject), the GTK
// bookmarks (~/.config/gtk-3.0/bookmarks, shared with GTK and Nautilus),
// and Other Locations. `places` rows: { section, name, icon, path, kind,
// tooltip, device, ejectable, removable }; kind is recent, home, folder,
// drive, bookmark or other.

#include <QFileSystemWatcher>
#include <QObject>
#include <QVariantList>

namespace atrium {

class ChooserPlaces : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool showRecent READ showRecent WRITE setShowRecent NOTIFY changed)
    Q_PROPERTY(QVariantList places READ places NOTIFY changed)
    // Other Locations: the computer's own disks ({ name, icon, path, device, mounted, size, free }).
    Q_PROPERTY(QVariantList otherLocations READ otherLocations NOTIFY changed)

public:
    explicit ChooserPlaces(QObject* parent = nullptr);

    bool showRecent() const { return showRecent_; }
    void setShowRecent(bool on);
    QVariantList places() const { return places_; }
    QVariantList otherLocations() const;

    Q_INVOKABLE bool isBookmarked(const QString& path) const;
    Q_INVOKABLE void addBookmark(const QString& path, int at = -1);
    Q_INVOKABLE void removeBookmark(const QString& path);
    Q_INVOKABLE void renameBookmark(const QString& path, const QString& label);
    Q_INVOKABLE void moveBookmark(const QString& path, int to);
    // Alt+1…Alt+0: the nth bookmark's folder ("" when there are fewer).
    Q_INVOKABLE QString nthBookmark(int n) const;
    // The sidebar row for where the list is (its path, or "recent").
    Q_INVOKABLE int rowFor(const QString& location) const;
    // Mounts a drive by its udisks object path, then calls back with where.
    Q_INVOKABLE void mount(const QString& device);
    Q_INVOKABLE void unmount(const QString& device);
    Q_INVOKABLE void eject(const QString& device);

    static QString bookmarksFile();

signals:
    void changed();
    void mounted(const QString& device, const QString& path);
    void failed(const QString& why);

private:
    void reload();
    void writeBookmarks(const QStringList& lines);
    QStringList bookmarkLines() const;

    bool showRecent_ = true;
    QVariantList places_;
    QFileSystemWatcher watcher_;
};

} // namespace atrium
