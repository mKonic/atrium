#pragma once
// The battery, through UPower: charge, what's left, a charge limit where
// the hardware has one, and a notice when it runs low. `present` is false
// on a desktop, and everything else then stays empty.

#include <QObject>

namespace atrium {

class Battery : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool present READ present NOTIFY changed)
    Q_PROPERTY(int percentage READ percentage NOTIFY changed)
    Q_PROPERTY(bool plugged READ plugged NOTIFY changed)
    Q_PROPERTY(QString glyph READ glyph NOTIFY changed)
    Q_PROPERTY(QString remaining READ remaining NOTIFY changed)  // "2:05 left"
    Q_PROPERTY(bool chargeLimitSupported READ chargeLimitSupported NOTIFY changed)
    Q_PROPERTY(bool chargeLimitEnabled READ chargeLimitEnabled NOTIFY changed)
    Q_PROPERTY(int chargeLimit READ chargeLimit NOTIFY changed)  // the percentage it stops at
    // logind's HandleLidSwitch ("suspend"), while this computer has a lid.
    Q_PROPERTY(QString lidAction READ lidAction NOTIFY changed)

public:
    static Battery* instance();

    bool present() const { return present_; }
    int percentage() const { return int(percent_ + 0.5); }
    bool plugged() const;
    QString glyph() const;
    QString remaining() const;
    bool chargeLimitSupported() const { return limitSupported_; }
    bool chargeLimitEnabled() const { return limitEnabled_; }
    int chargeLimit() const { return limitEnd_; }
    QString lidAction() const { return lidAction_; }

    Q_INVOKABLE void setChargeLimitEnabled(bool on);

signals:
    void changed();

private slots:
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    Battery();
    void findBattery();
    void load();
    void warn();

    QString device_;  // the battery's own object (charge limits live there)
    bool present_ = false, limitSupported_ = false, limitEnabled_ = false;
    double percent_ = 0;
    uint state_ = 0;
    qint64 toEmpty_ = 0, toFull_ = 0;
    int limitEnd_ = 0;
    int warned_ = 0;
    QString lidAction_;
};

} // namespace atrium
