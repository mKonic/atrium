#pragma once
// Disks through udisks2: removable drives (and internal ones outside the
// system) with mounting, unmounting and ejecting. The desktop shell mounts
// a drive that's plugged in and says so, with "Open" (disks.automount).
// `disks` is [{path, name, size, device, mounted, mountPoint, removable}].

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>

namespace atrium {

class Disks : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QVariantList disks READ disks NOTIFY changed)
    // Removable ones mounted now, for the menu bar's eject menu.
    Q_PROPERTY(QVariantList ejectable READ ejectable NOTIFY changed)

public:
    static Disks* instance();

    bool available() const { return available_; }
    QVariantList disks() const { return disks_; }
    QVariantList ejectable() const;

    Q_INVOKABLE void mount(const QString& path);
    Q_INVOKABLE void unmount(const QString& path);
    // Unmounted, then the drive powered off so it can be pulled out.
    Q_INVOKABLE void eject(const QString& path);
    Q_INVOKABLE void open(const QString& path);

signals:
    void changed();
    void failed(const QString& why);

private slots:
    void reloadSoon() { reload_.start(); }
    void actionInvoked(uint id, const QString& key);

private:
    Disks();
    void reload();
    void mounted(const QString& path, const QString& name, const QString& where);
    QString drive(const QString& path) const;

    bool available_ = false;
    bool loaded_ = false;     // the first load mounts nothing: those were there already
    QVariantList disks_;
    QSet<QString> seen_;
    QHash<uint, QString> notices_;  // notification id → mount point to open
    QTimer reload_;
};

} // namespace atrium
