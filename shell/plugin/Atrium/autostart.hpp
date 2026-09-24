#pragma once
// The apps that start at login (XDG autostart): the user's own in
// ~/.config/autostart and the system's in /etc/xdg/autostart, which the
// user turns off with a copy of their own marked Hidden, as every desktop
// does. `entries` is [{id, name, icon, enabled, own}], by name.

#include <QFileSystemWatcher>
#include <QObject>
#include <QVariantList>

namespace atrium {

class Autostart : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)

public:
    explicit Autostart(QObject* parent = nullptr);

    QVariantList entries() const { return entries_; }

    Q_INVOKABLE void setEnabled(const QString& id, bool on);
    // Installed apps whose name has `query` in it, not already here: [{id, name, icon}].
    Q_INVOKABLE QVariantList candidates(const QString& query) const;
    // An installed app (its desktop id) to start at login too.
    Q_INVOKABLE void add(const QString& appId);
    // Only the user's own; the system's can only be turned off.
    Q_INVOKABLE void remove(const QString& id);

signals:
    void changed();

private:
    void reload();
    QString userDir() const;

    QVariantList entries_;
    QFileSystemWatcher watcher_;
};

} // namespace atrium
