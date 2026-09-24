#pragma once
// The time, updated on the minute (or second, or hour) boundary rather than
// polled: `SystemClock { precision: SystemClock.Minutes }`, then `date`.

#include <QDateTime>
#include <QObject>
#include <QTimer>

namespace atrium::shell {

class SystemClock : public QObject {
    Q_OBJECT
    Q_PROPERTY(Precision precision READ precision WRITE setPrecision NOTIFY precisionChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QDateTime date READ date NOTIFY dateChanged)
    Q_PROPERTY(int hours READ hours NOTIFY dateChanged)
    Q_PROPERTY(int minutes READ minutes NOTIFY dateChanged)
    Q_PROPERTY(int seconds READ seconds NOTIFY dateChanged)

public:
    enum Precision { Hours, Minutes, Seconds };
    Q_ENUM(Precision)

    explicit SystemClock(QObject* parent = nullptr);

    Precision precision() const { return precision_; }
    void setPrecision(Precision p);
    bool enabled() const { return enabled_; }
    void setEnabled(bool on);
    QDateTime date() const { return date_; }
    int hours() const { return date_.time().hour(); }
    int minutes() const { return date_.time().minute(); }
    int seconds() const { return date_.time().second(); }

signals:
    void precisionChanged();
    void enabledChanged();
    void dateChanged();

public:
    // Now, not at the next tick (the time zone changed).
    Q_INVOKABLE void refresh() { tick(); }

private:
    void tick();

    Precision precision_ = Seconds;
    bool enabled_ = true;
    QDateTime date_;
    QTimer timer_;
};

} // namespace atrium::shell
