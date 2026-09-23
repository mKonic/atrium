#pragma once
// Screen brightness: external monitors over DDC/CI (ddcutil), laptop panels
// through their backlight (logind, no root). DDC is slow, so a moving slider
// only ever has one write in flight and the last value wins.

#include <QObject>
#include <QProcess>
#include <QVariant>

#include <optional>

namespace atrium {

class Brightness : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(int value READ value NOTIFY changed)  // 0..100, as last set or read
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    explicit Brightness(QObject* parent = nullptr);

    bool available() const { return !ddc_.empty() || !backlight_.isEmpty(); }
    int value() const { return value_; }
    bool busy() const { return busy_; }

    // Every screen to `percent`.
    Q_INVOKABLE void set(int percent);

signals:
    void changed();

private:
    void detect();
    void readCurrent();
    void writeNext();

    QString ddcutil_;
    std::vector<QString> ddc_;   // ddcutil display numbers
    QString backlight_;          // /sys/class/backlight/<name>
    int backlightMax_ = 0;
    int value_ = 100;
    bool busy_ = false;
    std::optional<int> pending_;  // the value to send once the current write ends
};

} // namespace atrium
