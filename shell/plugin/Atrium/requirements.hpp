#pragma once
// What Settings fronts but atrium doesn't provide itself: system services
// (NetworkManager, BlueZ, power-profiles-daemon, AccountsService), PipeWire,
// a Bluetooth adapter, and programs (cliphist, ddcutil, gpu-screen-recorder).
// `Requirements.missing` maps each one that's absent to a sentence saying so;
// present ones aren't in it. System services are watched, so the notice goes
// once one starts.

#include <QObject>
#include <QVariantMap>

namespace atrium {

class Requirements : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap missing READ missing NOTIFY missingChanged)

public:
    static Requirements* instance();

    QVariantMap missing() const { return missing_; }

signals:
    void missingChanged();

private slots:
    void nameOwnerChanged(const QString& name, const QString& before, const QString& after);

private:
    Requirements();
    void check();

    QVariantMap missing_;
};

} // namespace atrium
