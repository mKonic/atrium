#include "logind.hpp"

#include "lock_screen.hpp"
#include "server.hpp"

#ifdef ATRIUM_JOURNAL  // libsystemd
#include <systemd/sd-bus.h>
#endif

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace atrium {

#ifdef ATRIUM_JOURNAL

namespace {

constexpr const char* kLogind = "org.freedesktop.login1";
constexpr const char* kManagerPath = "/org/freedesktop/login1";
constexpr const char* kManager = "org.freedesktop.login1.Manager";
constexpr const char* kSession = "org.freedesktop.login1.Session";
constexpr int kSleepPoll = 50;     // ms between looks at the lock before sleep
constexpr int kSleepTries = 80;    // logind's own delay (InhibitDelayMaxSec) is 5 s

} // namespace

struct Logind::Impl {
    Server& server;
    sd_bus* bus = nullptr;
    std::string path;  // our session's object
    sd_bus_slot* slots[3] = {};
    wl_event_source* source = nullptr;
    wl_event_source* sleep_wait = nullptr;
    int inhibitor = -1;  // a sleep delay: held, sleep waits for the lock
    int power_key = -1;  // handle-power-key, blocked: the button is atrium's
    int sleep_tries = 0;
    bool sleeping = false;

    explicit Impl(Server& s) : server(s) {}

    ~Impl() {
        if (sleep_wait)
            wl_event_source_remove(sleep_wait);
        if (source)
            wl_event_source_remove(source);
        for (sd_bus_slot* s : slots)
            sd_bus_slot_unref(s);
        release();
        if (power_key >= 0)
            close(power_key);
        if (bus)
            sd_bus_flush_close_unref(bus);
    }

    bool open() {
        if (sd_bus_open_system(&bus) < 0) {
            bus = nullptr;
            return false;
        }
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const char* p = nullptr;
        if (sd_bus_call_method(bus, kLogind, kManagerPath, kManager, "GetSessionByPID", &err, &reply, "u",
                               uint32_t(getpid())) >= 0 &&
            sd_bus_message_read(reply, "o", &p) >= 0 && p)
            path = p;
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        if (path.empty())
            return false;  // not in a logind session (nested, a box)

        sd_bus_match_signal(bus, &slots[0], kLogind, path.c_str(), kSession, "Lock", [](sd_bus_message*, void* d, sd_bus_error*) {
            auto* self = static_cast<Impl*>(d);
            alog(Log::Info, "logind: lock");
            if (self->server.lock_screen)
                self->server.lock_screen->lock();
            return 0;
        }, this);
        sd_bus_match_signal(bus, &slots[1], kLogind, path.c_str(), kSession, "Unlock", [](sd_bus_message*, void* d, sd_bus_error*) {
            auto* self = static_cast<Impl*>(d);
            alog(Log::Info, "logind: unlock");
            if (self->server.lock_screen)
                self->server.lock_screen->unlock();
            return 0;
        }, this);
        sd_bus_match_signal(bus, &slots[2], kLogind, kManagerPath, kManager, "PrepareForSleep", [](sd_bus_message* m, void* d, sd_bus_error*) {
            int start = 0;
            if (sd_bus_message_read(m, "b", &start) >= 0)
                static_cast<Impl*>(d)->prepare_for_sleep(start);
            return 0;
        }, this);

        source = wl_event_loop_add_fd(server.loop, sd_bus_get_fd(bus), WL_EVENT_READABLE, [](int, uint32_t, void* d) {
            static_cast<Impl*>(d)->process();
            return 0;
        }, this);
        sleep_wait = wl_event_loop_add_timer(server.loop, [](void* d) {
            static_cast<Impl*>(d)->check_sleep_lock();
            return 0;
        }, this);
        alog(Log::Info, "logind: session %s", path.c_str());
        return true;
    }

    // Everything waiting, also what a blocking call read past.
    void process() {
        while (sd_bus_process(bus, nullptr) > 0) {
        }
    }

    // After a call of ours: its reply may have brought signals with it,
    // which the fd won't announce again.
    void process_soon() {
        wl_event_loop_add_idle(server.loop, [](void* d) { static_cast<Impl*>(d)->process(); }, this);
    }

    // The fd of a logind inhibitor, or -1.
    int inhibit(const char* what, const char* why, const char* mode) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int fd = -1, out = -1;
        if (sd_bus_call_method(bus, kLogind, kManagerPath, kManager, "Inhibit", &err, &reply, "ssss", what, "atrium",
                               why, mode) >= 0 &&
            sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0)
            out = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        else
            alog(Log::Error, "logind: no %s inhibitor: %s", what, err.message ? err.message : "?");
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        process_soon();
        return out;
    }

    void take() {
        if (inhibitor >= 0 || !server.config.lock_before_sleep || !server.lock_screen)
            return;
        inhibitor = inhibit("sleep", "Locking the screen first", "delay");
    }

    void release() {
        if (inhibitor >= 0)
            close(inhibitor);
        inhibitor = -1;
    }

    void prepare_for_sleep(bool start) {
        sleeping = start;
        if (!start) {
            sleep_tries = 0;
            take();
            return;
        }
        if (server.config.lock_before_sleep && server.lock_screen)
            server.lock_screen->lock();
        sleep_tries = 0;
        check_sleep_lock();
    }

    // Sleep goes ahead once the lock is in place (or it never will be).
    void check_sleep_lock() {
        if (!sleeping || inhibitor < 0)
            return;
        const bool locked = server.locked && server.lock;
        if (locked || !server.config.lock_before_sleep || ++sleep_tries >= kSleepTries) {
            if (!locked && server.config.lock_before_sleep)
                alog(Log::Error, "logind: sleeping before the screen locked");
            release();
            return;
        }
        wl_event_source_timer_update(sleep_wait, kSleepPoll);
    }
};

Logind::Logind(Server& server) : impl_(std::make_unique<Impl>(server)) {
    if (!impl_->open()) {
        impl_.reset();
        return;
    }
    impl_->take();
    impl_->power_key = impl_->inhibit("handle-power-key", "Asks before shutting down", "block");
}

bool Logind::holds_power_key() const {
    return impl_ && impl_->power_key >= 0;
}

namespace {

void manager_call(sd_bus* bus, const char* method) {
    sd_bus_call_method_async(bus, nullptr, kLogind, kManagerPath, kManager, method, [](sd_bus_message* m, void* d,
                                                                                         sd_bus_error*) {
        if (const sd_bus_error* e = sd_bus_message_get_error(m))
            alog(Log::Error, "logind: %s refused: %s", static_cast<const char*>(d), e->message ? e->message : e->name);
        return 0;
    }, const_cast<char*>(method), "b", 1);
    sd_bus_flush(bus);
}

} // namespace

void Logind::power_off() {
    if (impl_)
        manager_call(impl_->bus, "PowerOff");
}

void Logind::reboot() {
    if (impl_)
        manager_call(impl_->bus, "Reboot");
}

Logind::~Logind() = default;

void Logind::set_locked_hint(bool locked) {
    if (!impl_)
        return;
    sd_bus_call_method_async(impl_->bus, nullptr, kLogind, impl_->path.c_str(), kSession, "SetLockedHint", nullptr,
                             nullptr, "b", int(locked));
    sd_bus_flush(impl_->bus);
}

void Logind::set_idle_hint(bool idle) {
    if (!impl_)
        return;
    sd_bus_call_method_async(impl_->bus, nullptr, kLogind, impl_->path.c_str(), kSession, "SetIdleHint", nullptr,
                             nullptr, "b", int(idle));
    sd_bus_flush(impl_->bus);
}

bool Logind::idle_blocked() {
    if (!impl_)
        return false;
    char* what = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    bool blocked = false;
    if (sd_bus_get_property_string(impl_->bus, kLogind, kManagerPath, kManager, "BlockInhibited", &err, &what) >= 0 &&
        what) {
        // A colon-separated list: "idle" among "sleep:shutdown:idle:...".
        const std::string list = std::string(":") + what + ":";
        blocked = list.find(":idle:") != std::string::npos;
    }
    free(what);
    sd_bus_error_free(&err);
    impl_->process_soon();
    return blocked;
}

void Logind::suspend() {
    if (!impl_)
        return;
    sd_bus_call_method_async(impl_->bus, nullptr, kLogind, kManagerPath, kManager, "Suspend", nullptr, nullptr, "b",
                             0);
    sd_bus_flush(impl_->bus);
}

void Logind::reconfigure() {
    if (!impl_)
        return;
    if (impl_->server.config.lock_before_sleep)
        impl_->take();
    else
        impl_->release();
}

#else

struct Logind::Impl {};
Logind::Logind(Server&) {}
Logind::~Logind() = default;
void Logind::set_locked_hint(bool) {}
void Logind::reconfigure() {}
void Logind::set_idle_hint(bool) {}
bool Logind::idle_blocked() { return false; }
void Logind::suspend() {}
void Logind::power_off() {}
void Logind::reboot() {}
bool Logind::holds_power_key() const { return false; }

#endif

} // namespace atrium
