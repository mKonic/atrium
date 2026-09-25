#pragma once
// Screen brightness: external monitors over DDC/CI (written straight to their
// I2C bus), laptop panels through their backlight (logind, no root). With HDR
// on it is atrium's SDR brightness instead: the screen's backlight is fixed
// then. DDC is slow, so a moving slider only ever has one write in flight and
// the last value wins.

#include <QObject>
#include <QProcess>
#include <QVariant>

#include <optional>
#include <vector>

namespace atrium {

class Brightness : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(int value READ value NOTIFY changed)  // 0..100, as last set or read
    Q_PROPERTY(bool hdr READ hdrActive NOTIFY changed)

public:
    explicit Brightness(QObject* parent = nullptr);

    bool available() const;
    int value() const;
    bool hdrActive() const { return hdr_; }

    // Every screen to `percent`, as a slider moves.
    Q_INVOKABLE void set(int percent);
    // Let go at `percent`: remembered (the Brightness setting, or the
    // display's SDR brightness in HDR).
    Q_INVOKABLE void commit(int percent);

signals:
    void changed();

private:
    void detect();
    void readCurrent();
    void setBacklight(int percent);
    void writeNext();
    bool hdr() const;
    std::optional<int> setting() const;
    void applySetting();

    QString ddcutil_;
    std::vector<int> buses_;     // /dev/i2c-N of each monitor that answers DDC/CI
    int max_ = 100;              // the monitors' own top value
    QString backlight_;          // /sys/class/backlight/<name>
    int backlightMax_ = 0;
    int value_ = 100;
    bool hdr_ = false;
    bool busy_ = false;
    std::optional<int> pending_;  // the value to send once the current write ends
    std::optional<int> seen_;     // the setting as last seen
};

} // namespace atrium
