#pragma once
// Download and upload speed, sampled once a second and averaged over a few
// (netspeed::Smoother) while something shows it: `NetSpeed { active: shown }`.

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <cstdint>

#include "netspeed_core.hpp"

namespace atrium {

class NetSpeed : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(double down READ down NOTIFY changed)  // bytes per second
    Q_PROPERTY(double up READ up NOTIFY changed)
    Q_PROPERTY(QString downText READ downText NOTIFY changed)
    Q_PROPERTY(QString upText READ upText NOTIFY changed)

public:
    explicit NetSpeed(QObject* parent = nullptr);

    bool active() const { return timer_.isActive(); }
    void setActive(bool active);
    double down() const { return down_.rate(); }
    double up() const { return up_.rate(); }
    QString downText() const { return downText_; }
    QString upText() const { return upText_; }

signals:
    void activeChanged();
    void changed();

private:
    void sample();

    QTimer timer_;
    QElapsedTimer clock_;
    uint64_t rx_ = 0, tx_ = 0;
    bool primed_ = false;
    netspeed::Smoother down_, up_;
    QString downText_ = "0 KB/s", upText_ = "0 KB/s";
};

} // namespace atrium
