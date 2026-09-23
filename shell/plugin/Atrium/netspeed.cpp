#include "netspeed.hpp"

#include "netspeed_core.hpp"

#include <QFile>

namespace atrium {

NetSpeed::NetSpeed(QObject* parent) : QObject(parent) {
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &NetSpeed::sample);
}

void NetSpeed::setActive(bool active) {
    if (active == timer_.isActive())
        return;
    if (active) {
        primed_ = false;
        sample();
        timer_.start();
    } else {
        timer_.stop();
    }
    emit activeChanged();
}

void NetSpeed::sample() {
    QFile f("/proc/net/dev");
    if (!f.open(QIODevice::ReadOnly))
        return;
    const netspeed::Totals t = netspeed::read_totals(f.readAll().toStdString());
    const double seconds = clock_.isValid() ? clock_.restart() / 1000.0 : (clock_.start(), 0.0);
    if (primed_ && seconds > 0) {
        // Counters that went backwards (an interface went away) read as idle.
        down_ = t.rx >= rx_ ? (t.rx - rx_) / seconds : 0;
        up_ = t.tx >= tx_ ? (t.tx - tx_) / seconds : 0;
        downText_ = QString::fromStdString(netspeed::format_rate(down_));
        upText_ = QString::fromStdString(netspeed::format_rate(up_));
        emit changed();
    }
    rx_ = t.rx;
    tx_ = t.tx;
    primed_ = true;
}

} // namespace atrium
