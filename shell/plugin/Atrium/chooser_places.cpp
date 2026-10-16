#include "chooser_places.hpp"

#include "chooser_core.hpp"
#include "disks.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>

namespace atrium {

namespace {

QString pathFor(const std::string& uri) {
    const auto p = chooser::path_for_uri(uri);
    return p ? QString::fromStdString(*p) : QString();
}

} // namespace

ChooserPlaces::ChooserPlaces(QObject* parent) : QObject(parent) {
    QDir().mkpath(QFileInfo(bookmarksFile()).path());
    watcher_.addPath(QFileInfo(bookmarksFile()).path());
    if (QFileInfo::exists(bookmarksFile()))
        watcher_.addPath(bookmarksFile());
    auto again = [this] {
        if (QFileInfo::exists(bookmarksFile()) && !watcher_.files().contains(bookmarksFile()))
            watcher_.addPath(bookmarksFile());
        reload();
    };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, again);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, again);
    connect(Disks::instance(), &Disks::changed, this, &ChooserPlaces::reload);
    connect(Disks::instance(), &Disks::failed, this, &ChooserPlaces::failed);
    reload();
}

QString ChooserPlaces::bookmarksFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/gtk-3.0/bookmarks";
}

void ChooserPlaces::setShowRecent(bool on) {
    if (on == showRecent_)
        return;
    showRecent_ = on;
    reload();
}

QStringList ChooserPlaces::bookmarkLines() const {
    QFile f(bookmarksFile());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QStringList out;
    for (const chooser::Bookmark& b : chooser::parse_bookmarks(f.readAll().toStdString()))
        out << QString::fromStdString(b.uri) + (b.label.empty() ? QString() : " " + QString::fromStdString(b.label));
    return out;
}

void ChooserPlaces::writeBookmarks(const QStringList& lines) {
    QSaveFile f(bookmarksFile());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write((lines.join('\n') + (lines.isEmpty() ? "" : "\n")).toUtf8());
    f.commit();
    reload();
}

void ChooserPlaces::reload() {
    QVariantList out;
    const QString home = QDir::homePath();
    // GTK names an XDG folder by its bookmark's label when it has one.
    QHash<QString, QString> labels;
    QStringList bookmarked;
    for (const QString& line : bookmarkLines()) {
        const auto b = chooser::parse_bookmarks(line.toStdString());
        if (b.empty())
            continue;
        const QString path = pathFor(b[0].uri);
        if (path.isEmpty())
            continue;  // local only
        labels.insert(path, QString::fromStdString(b[0].label));
        bookmarked << path;
    }
    auto add = [&](const QString& section, const QString& name, const QString& icon, const QString& path,
                   const QString& kind, const QString& tooltip, const QVariantMap& extra = {}) {
        QVariantMap row{{"section", section}, {"name", name}, {"icon", icon}, {"path", path},
                        {"kind", kind},       {"tooltip", tooltip}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            row.insert(it.key(), it.value());
        out.append(row);
    };

    if (showRecent_)
        add("computer", "Recent", "schedule", "recent", "recent", "Recent files");
    add("computer", "Home", "home", home, "home", "Open your personal folder");
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop != home && QFileInfo(desktop).isDir())
        add("computer", "Desktop", "desktop_windows", desktop, "folder", "Open the contents of your desktop in a folder");
    // The XDG folders GTK lists (not Templates or Public), each once, never home.
    using SP = QStandardPaths;
    QStringList seen{home, desktop};
    for (auto [where, icon] : {std::pair{SP::DocumentsLocation, "description"}, std::pair{SP::DownloadLocation, "download"},
                               std::pair{SP::MusicLocation, "music_note"}, std::pair{SP::PicturesLocation, "image"},
                               std::pair{SP::MoviesLocation, "movie"}}) {
        const QString path = SP::writableLocation(where);
        if (path.isEmpty() || seen.contains(path) || !QFileInfo(path).isDir())
            continue;
        seen << path;
        const QString label = labels.value(path);
        add("computer", label.isEmpty() ? QFileInfo(path).fileName() : label, icon, path, "folder", path);
    }

    // Removable drives, mounted or not (a click mounts); internal ones are under Other Locations.
    for (const QVariant& v : Disks::instance()->disks()) {
        const QVariantMap d = v.toMap();
        if (!d.value("removable").toBool())
            continue;
        const bool on = d.value("mounted").toBool();
        add("mounts", d.value("name").toString(), "usb", d.value("mountPoint").toString(), "drive",
            on ? d.value("mountPoint").toString() : QString("Mount and open “%1”").arg(d.value("name").toString()),
            {{"device", d.value("path")}, {"mounted", on}, {"ejectable", on}});
    }

    // Bookmarks, in the file's order, except ones already listed above.
    for (const QString& path : bookmarked) {
        if (seen.contains(path))
            continue;
        const QString label = labels.value(path);
        const bool there = QFileInfo(path).isDir();
        add("bookmarks", label.isEmpty() ? QFileInfo(path).fileName() : label, there ? "folder" : "folder_off", path,
            "bookmark", path, {{"missing", !there}});
    }

    add("other", "Other Locations", "add", "other", "other", "Show other locations");
    places_ = out;
    emit changed();
}

QVariantList ChooserPlaces::otherLocations() const {
    // GtkPlacesView's "On This Computer": the computer itself and its own disks.
    QVariantList out;
    const QLocale locale;
    const QStorageInfo root = QStorageInfo::root();
    out.append(QVariantMap{{"name", "Computer"}, {"icon", "computer"}, {"path", "/"}, {"mounted", true},
                           {"free", locale.formattedDataSize(root.bytesAvailable(), 1, QLocale::DataSizeSIFormat)},
                           {"size", locale.formattedDataSize(root.bytesTotal(), 1, QLocale::DataSizeSIFormat)}});
    for (const QVariant& v : Disks::instance()->disks()) {
        const QVariantMap d = v.toMap();
        if (d.value("removable").toBool())
            continue;
        const QString where = d.value("mountPoint").toString();
        QVariantMap row{{"name", d.value("name")}, {"icon", "hard_drive"}, {"path", where},
                        {"device", d.value("path")}, {"mounted", d.value("mounted")}, {"size", d.value("size")}};
        if (!where.isEmpty()) {
            const QStorageInfo s(where);
            row.insert("free", locale.formattedDataSize(s.bytesAvailable(), 1, QLocale::DataSizeSIFormat));
        }
        out.append(row);
    }
    return out;
}

bool ChooserPlaces::isBookmarked(const QString& path) const {
    const QString uri = QString::fromStdString(chooser::uri_for_path(path.toStdString()));
    for (const QString& line : bookmarkLines())
        if (line == uri || line.startsWith(uri + " "))
            return true;
    return false;
}

void ChooserPlaces::addBookmark(const QString& path, int at) {
    if (path.isEmpty() || isBookmarked(path))
        return;
    QStringList lines = bookmarkLines();
    const QString uri = QString::fromStdString(chooser::uri_for_path(path.toStdString()));
    if (at < 0 || at > lines.size())
        lines << uri;
    else
        lines.insert(at, uri);
    writeBookmarks(lines);
}

void ChooserPlaces::removeBookmark(const QString& path) {
    const QString uri = QString::fromStdString(chooser::uri_for_path(path.toStdString()));
    QStringList lines = bookmarkLines();
    lines.removeIf([&](const QString& l) { return l == uri || l.startsWith(uri + " "); });
    writeBookmarks(lines);
}

void ChooserPlaces::renameBookmark(const QString& path, const QString& label) {
    const QString uri = QString::fromStdString(chooser::uri_for_path(path.toStdString()));
    QStringList lines = bookmarkLines();
    for (QString& l : lines)
        if (l == uri || l.startsWith(uri + " "))
            l = label.trimmed().isEmpty() ? uri : uri + " " + label.trimmed();
    writeBookmarks(lines);
}

void ChooserPlaces::moveBookmark(const QString& path, int to) {
    const QString uri = QString::fromStdString(chooser::uri_for_path(path.toStdString()));
    QStringList lines = bookmarkLines();
    const qsizetype from = lines.indexOf(uri) >= 0 ? lines.indexOf(uri) : [&] {
        for (qsizetype i = 0; i < lines.size(); ++i)
            if (lines[i].startsWith(uri + " "))
                return i;
        return qsizetype(-1);
    }();
    if (from < 0)
        return;
    lines.move(from, std::clamp<qsizetype>(to, 0, lines.size() - 1));
    writeBookmarks(lines);
}

QString ChooserPlaces::nthBookmark(int n) const {
    int i = 0;
    for (const QVariant& v : places_) {
        const QVariantMap p = v.toMap();
        if (p.value("kind") != "bookmark")
            continue;
        if (i++ == n)
            return p.value("path").toString();
    }
    return {};
}

int ChooserPlaces::rowFor(const QString& location) const {
    for (qsizetype i = 0; i < places_.size(); ++i)
        if (places_[i].toMap().value("path") == location)
            return int(i);
    return -1;
}

void ChooserPlaces::mount(const QString& device) {
    // Once udisks reports it mounted, say where.
    auto* c = new QMetaObject::Connection;
    *c = connect(Disks::instance(), &Disks::changed, this, [this, c, device] {
        for (const QVariant& v : Disks::instance()->disks()) {
            const QVariantMap d = v.toMap();
            if (d.value("path") == device && d.value("mounted").toBool()) {
                disconnect(*c);
                delete c;
                emit mounted(device, d.value("mountPoint").toString());
                return;
            }
        }
    });
    Disks::instance()->mount(device);
}

void ChooserPlaces::unmount(const QString& device) {
    Disks::instance()->unmount(device);
}

void ChooserPlaces::eject(const QString& device) {
    Disks::instance()->eject(device);
}

} // namespace atrium
