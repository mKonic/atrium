#pragma once
// Printers through CUPS' command-line tools: the ones set up and the ones
// the network offers (driverless, printable as they are), each with its
// queue; the default; adding one by its address and removing one (these
// two as the administrator, through pkexec). `printers` is
// [{name, label, state, isDefault, installed, jobs: [{id, user, size}]}].

#include <QObject>
#include <QTimer>
#include <QVariantList>

namespace atrium {

class Printers : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList printers READ printers NOTIFY changed)
    // While true (the page is open), the list and queues are looked at every few seconds.
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged)

public:
    explicit Printers(QObject* parent = nullptr);

    QVariantList printers() const { return printers_; }
    bool watching() const { return timer_.isActive(); }
    void setWatching(bool on);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setDefault(const QString& name);
    Q_INVOKABLE void cancel(const QString& job);
    // "192.168.1.20" or an ipp:// address: a driverless (IPP Everywhere) printer.
    Q_INVOKABLE void add(const QString& name, const QString& address);
    Q_INVOKABLE void remove(const QString& name);

signals:
    void changed();
    void watchingChanged();
    void failed(const QString& why);

private:
    void run(const QString& program, const QStringList& args);

    QVariantList printers_;
    QTimer timer_;
};

} // namespace atrium
