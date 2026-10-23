#include "wallpaper.hpp"

#include "compositor.hpp"
#include "wallpaper_core.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
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

bool Wallpaper::darkTop(const QString& path) const {
    if (path == toneOf_)
        return dark_;
    toneOf_ = path;
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid())
        return dark_ = true;
    // Small is plenty for a mean (JPEG decodes straight to it), and the top
    // twentieth is where the bar is.
    reader.setScaledSize(size.scaled(64, 64, Qt::KeepAspectRatio));
    const QImage image = reader.read().convertToFormat(QImage::Format_RGB32);
    if (image.isNull())
        return dark_ = true;
    const int rows = std::max(1, image.height() / 20);
    double sum = 0;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = image.pixel(x, y);
            sum += 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c);
        }
    return dark_ = sum / (rows * image.width()) < 150;
}

} // namespace atrium
