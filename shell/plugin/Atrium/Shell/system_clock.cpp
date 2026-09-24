#include "system_clock.hpp"

namespace atrium::shell {

SystemClock::SystemClock(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &SystemClock::tick);
    tick();
}

void SystemClock::setPrecision(Precision p) {
    if (p == precision_)
        return;
    precision_ = p;
    emit precisionChanged();
    tick();
}

void SystemClock::setEnabled(bool on) {
    if (on == enabled_)
        return;
    enabled_ = on;
    emit enabledChanged();
    if (on)
        tick();
    else
        timer_.stop();
}

void SystemClock::tick() {
    const QDateTime now = QDateTime::currentDateTime();
    QTime t = now.time();
    // Truncated to the precision, so a binding only changes when it shows.
    if (precision_ != Seconds)
        t = QTime(t.hour(), precision_ == Minutes ? t.minute() : 0);
    else
        t = QTime(t.hour(), t.minute(), t.second());
    const QDateTime shown(now.date(), t);
    if (shown != date_) {
        date_ = shown;
        emit dateChanged();
    }
    if (!enabled_)
        return;
    // Wake just after the next boundary (a suspend is caught by the next
    // boundary after resume, within one unit).
    const qint64 unit = precision_ == Seconds ? 1000 : precision_ == Minutes ? 60'000 : 3'600'000;
    const qint64 ms = now.toMSecsSinceEpoch() + now.offsetFromUtc() * 1000LL;
    timer_.start(int(unit - ms % unit + 5));
}

} // namespace atrium::shell
