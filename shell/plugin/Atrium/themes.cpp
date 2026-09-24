#include "themes.hpp"

#include "ini_core.hpp"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QMap>
#include <QStandardPaths>

#include <algorithm>

namespace atrium {

namespace {

QVariantList sorted(const QMap<QString, QString>& byValue) {
    QVariantList out;
    for (auto it = byValue.begin(); it != byValue.end(); ++it)
        out.append(QVariantMap{{"value", it.key()}, {"label", it.value()}});
    QCollator order;
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(out.begin(), out.end(), [&](const QVariant& a, const QVariant& b) {
        return order.compare(a.toMap().value("label").toString(), b.toMap().value("label").toString()) < 0;
    });
    out.prepend(QVariantMap{{"value", QString()}, {"label", QStringLiteral("Default")}});
    return out;
}

QStringList iconDirs() {
    QStringList dirs = {QDir::homePath() + "/.icons"};
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        dirs.append(d + "/icons");
    return dirs;
}

} // namespace

Themes::Themes(QObject* parent) : QObject(parent) {
    QMap<QString, QString> icons, cursors;
    for (const QString& dir : iconDirs())
        for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString id = fi.fileName();
            QFile f(fi.filePath() + "/index.theme");
            const std::string text = f.open(QIODevice::ReadOnly) ? f.readAll().toStdString() : std::string();
            const auto name = ini::get(text, "Icon Theme", "Name");
            const QString label = name ? QString::fromStdString(*name) : id;
            if (QDir(fi.filePath() + "/cursors").exists() && !cursors.contains(id))
                cursors.insert(id, label);
            // An icon theme has icons (Directories), and isn't one meant to stay out of lists.
            if (ini::get(text, "Icon Theme", "Directories") && ini::get(text, "Icon Theme", "Hidden") != "true" &&
                id != "hicolor" && !icons.contains(id))
                icons.insert(id, label);
        }
    icons_ = sorted(icons);
    cursors_ = sorted(cursors);
}

QVariantList Themes::fonts() const {
    QMap<QString, QString> all;
    for (const QString& f : QFontDatabase::families())
        if (!QFontDatabase::isPrivateFamily(f))
            all.insert(f, f);
    return sorted(all);
}

QVariantList Themes::monospaceFonts() const {
    QMap<QString, QString> mono;
    for (const QString& f : QFontDatabase::families())
        if (!QFontDatabase::isPrivateFamily(f) && QFontDatabase::isFixedPitch(f))
            mono.insert(f, f);
    return sorted(mono);
}

QVariantList Themes::optionsFor(const QString& key) const {
    if (key == "appearance.icon_theme")
        return iconThemes();
    if (key == "cursor.theme")
        return cursorThemes();
    if (key == "appearance.font")
        return fonts();
    if (key == "appearance.monospace_font")
        return monospaceFonts();
    return {};
}

} // namespace atrium
