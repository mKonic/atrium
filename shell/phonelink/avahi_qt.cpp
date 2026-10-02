#include "avahi_qt.hpp"

#include <QSocketNotifier>
#include <QTimer>

#include <sys/time.h>

#include <memory>

// Avahi's poll API takes C structs by these names.
struct AvahiWatch {
    int fd;
    AvahiWatchEvent events = AvahiWatchEvent(0), last = AvahiWatchEvent(0);
    AvahiWatchCallback callback;
    void* userdata;
    std::unique_ptr<QSocketNotifier> read, write;

    void set(AvahiWatchEvent e) {
        events = e;
        read->setEnabled(e & AVAHI_WATCH_IN);
        write->setEnabled(e & AVAHI_WATCH_OUT);
    }
    void fire(AvahiWatchEvent e) {
        last = e;
        callback(this, fd, e, userdata);
    }
};

struct AvahiTimeout {
    QTimer* timer = new QTimer;
    AvahiTimeoutCallback callback;
    void* userdata;

    void set(const struct timeval* tv) {
        if (!tv) {
            timer->stop();
            return;
        }
        timeval now;
        gettimeofday(&now, nullptr);
        const long long ms = (tv->tv_sec - now.tv_sec) * 1000LL + (tv->tv_usec - now.tv_usec) / 1000;
        timer->start(int(std::max(0LL, ms)));
    }
};

namespace atrium::phonelink {

namespace {

AvahiWatch* watchNew(const AvahiPoll*, int fd, AvahiWatchEvent events, AvahiWatchCallback cb, void* userdata) {
    auto* w = new AvahiWatch{fd, AvahiWatchEvent(0), AvahiWatchEvent(0), cb, userdata, {}, {}};
    w->read = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    w->write = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Write);
    QObject::connect(w->read.get(), &QSocketNotifier::activated, [w] { w->fire(AVAHI_WATCH_IN); });
    QObject::connect(w->write.get(), &QSocketNotifier::activated, [w] { w->fire(AVAHI_WATCH_OUT); });
    w->set(events);
    return w;
}

void watchUpdate(AvahiWatch* w, AvahiWatchEvent events) { w->set(events); }

AvahiWatchEvent watchGetEvents(AvahiWatch* w) { return w->last; }

// Avahi frees from inside callbacks: the notifiers go once their signal is done.
void watchFree(AvahiWatch* w) {
    for (auto* n : {w->read.release(), w->write.release()}) {
        n->setEnabled(false);
        n->disconnect();
        n->deleteLater();
    }
    delete w;
}

AvahiTimeout* timeoutNew(const AvahiPoll*, const struct timeval* tv, AvahiTimeoutCallback cb, void* userdata) {
    auto* t = new AvahiTimeout{new QTimer, cb, userdata};
    t->timer->setSingleShot(true);
    QObject::connect(t->timer, &QTimer::timeout, [t] { t->callback(t, t->userdata); });
    t->set(tv);
    return t;
}

void timeoutUpdate(AvahiTimeout* t, const struct timeval* tv) { t->set(tv); }

void timeoutFree(AvahiTimeout* t) {
    t->timer->stop();
    t->timer->disconnect();
    t->timer->deleteLater();
    delete t;
}

} // namespace

const AvahiPoll* qtAvahiPoll() {
    static const AvahiPoll poll{
        nullptr, watchNew, watchUpdate, watchGetEvents, watchFree, timeoutNew, timeoutUpdate, timeoutFree,
    };
    return &poll;
}

} // namespace atrium::phonelink
