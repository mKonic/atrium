#include "wallpaper.hpp"

#include "compositor.hpp"
#include "wallpaper_core.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QStandardPaths>

#include <functional>

namespace atrium {

namespace {

std::string read(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll().toStdString();
}

// A picture Qt can show, not a leftover name.
bool usable(const QString& path) {
    return !path.isEmpty() && QFileInfo(path).isFile() && QImageReader(path).canRead();
}

} // namespace

Wallpaper::Wallpaper(QObject* parent) : QObject(parent) {}

QString Wallpaper::found() const {
    look();
    return found_;
}

QString Wallpaper::foundFrom() const {
    look();
    return foundFrom_;
}

void Wallpaper::look() const {
    if (looked_)
        return;
    looked_ = true;
    const QString home = QDir::homePath();
    const std::string h = home.toStdString();
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString state = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);

    // Most specific first: shells and wallpaper tools, then whole desktops.
    const QList<std::pair<QString, std::function<std::string()>>> sources = {
        {"caelestia", [&] {
             return wallpaper::clean_path(read(state + "/caelestia/wallpaper/path.txt"), h);
         }},
        {"Hyprland", [&] { return wallpaper::from_hyprpaper(read(config + "/hypr/hyprpaper.conf"), h); }},
        {"waypaper", [&] { return wallpaper::from_waypaper(read(config + "/waypaper/config.ini"), h); }},
        {"Plasma", [&] {
             return wallpaper::from_plasma(read(config + "/plasma-org.kde.plasma.desktop-appletsrc"), h);
         }},
        {"GNOME", [&] {
             if (QStandardPaths::findExecutable("gsettings").isEmpty())
                 return std::string();
             QProcess p;
             p.start("gsettings", {"get", "org.gnome.desktop.background", "picture-uri"});
             p.waitForFinished(2000);
             return wallpaper::clean_path(p.readAllStandardOutput().trimmed().toStdString(), h);
         }},
    };
    for (const auto& [from, find] : sources) {
        const QString path = QString::fromStdString(find());
        if (usable(path)) {
            found_ = path;
            foundFrom_ = from;
            return;
        }
    }
}

QUrl Wallpaper::url(const QString& path) const {
    return QFileInfo::exists(path) ? QUrl::fromLocalFile(path) : QUrl();
}

void Wallpaper::set(const QUrl& file) {
    setPath(file.toLocalFile());
}

void Wallpaper::setPath(const QString& path) {
    Compositor::instance()->setSetting("appearance.wallpaper", path);
}

} // namespace atrium
