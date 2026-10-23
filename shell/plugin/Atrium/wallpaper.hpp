#pragma once
// The desktop's picture: `Wallpaper.url(path)` for an Image (empty when the
// file is gone), `Wallpaper.set(fileUrl)` from a file dialog, and `found`, a
// wallpaper another desktop on this computer already uses, to bring along.
// `darkTop(path)`: whether the strip the menu bar sits on is dark, so a bar
// without a background draws light text over it, as macOS's does.

#include <QObject>
#include <QUrl>

namespace atrium {

class Wallpaper : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString found READ found CONSTANT)          // a path, or ""
    Q_PROPERTY(QString foundFrom READ foundFrom CONSTANT)  // "Plasma", "Hyprland", ...

public:
    explicit Wallpaper(QObject* parent = nullptr);

    QString found() const;
    QString foundFrom() const;

    Q_INVOKABLE QUrl url(const QString& path) const;
    Q_INVOKABLE void set(const QUrl& file);
    Q_INVOKABLE void setPath(const QString& path);
    Q_INVOKABLE bool darkTop(const QString& path) const;

private:
    void look() const;  // on first asking: it may run gsettings

    mutable bool looked_ = false;
    mutable QString found_, foundFrom_;
    mutable QString toneOf_;
    mutable bool dark_ = true;
};

} // namespace atrium
