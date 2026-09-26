#include "sync.hpp"

#include "bluetooth.hpp"
#include "compositor.hpp"
#include "rfcomm.hpp"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QLoggingCategory>
#include <QSysInfo>

#include <cerrno>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace atrium::clipsync {

namespace {

Q_LOGGING_CATEGORY(lc, "atrium.clipsync")

const QString kSetting = QStringLiteral("bluetooth.phone_clipboard");
// Waits between tries at a phone that is connected but whose module didn't
// answer (not installed, or still starting after a reboot).
constexpr int kRetry[] = {3'000, 10'000, 30'000, 60'000};

} // namespace

// --- Link --------------------------------------------------------------------

Link::Link(Sync& sync, const QString& device, const QString& name, std::vector<Clip> recent, int fd)
    : sync_(sync), device_(device), fd_(fd), session_(name.toStdString(), std::move(recent)) {
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    read_ = std::make_unique<QSocketNotifier>(fd_, QSocketNotifier::Read);
    write_ = std::make_unique<QSocketNotifier>(fd_, QSocketNotifier::Write);
    write_->setEnabled(false);
    connect(read_.get(), &QSocketNotifier::activated, this, &Link::readable);
    connect(write_.get(), &QSocketNotifier::activated, this, &Link::pump);
}

Link::~Link() {
    ::close(fd_);
}

void Link::apply(const std::vector<Action>& actions) {
    for (const Action& a : actions) {
        switch (a.kind) {
        case Action::Kind::Send:
            out_ += a.bytes;
            break;
        case Action::Kind::SetClipboard:
            sync_.setClipboard(this, a.clip);
            break;
        case Action::Kind::AddHistory:
            sync_.addHistory(a.clip);
            break;
        case Action::Kind::Close:
            close();
            return;
        }
    }
    pump();
}

void Link::readable() {
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n > 0) {
            const bool wasSynced = session_.synced();
            const auto actions = session_.received(std::string_view(buf, std::size_t(n)));
            apply(actions);
            if (closed_)
                return;
            if (!wasSynced && session_.synced()) {
                bool took = false;
                for (const Action& a : actions)
                    took |= a.kind == Action::Kind::SetClipboard;
                qCInfo(lc) << "synced with" << QString::fromStdString(session_.peerName());
                sync_.merged(took);
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        close();  // the phone went away
        return;
    }
}

void Link::pump() {
    while (!out_.empty()) {
        const ssize_t n = ::write(fd_, out_.data(), out_.size());
        if (n > 0) {
            out_.erase(0, std::size_t(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            write_->setEnabled(true);
            return;
        }
        close();
        return;
    }
    write_->setEnabled(false);
}

void Link::close() {
    if (std::exchange(closed_, true))
        return;
    read_->setEnabled(false);
    write_->setEnabled(false);
    emit closed();
}

// --- Status ------------------------------------------------------------------

void Status::set(const QString& state, const QString& phone) {
    if (state == state_ && phone == phone_)
        return;
    state_ = state;
    phone_ = phone;
    emit StatusChanged(state_, phone_);
}

// --- Sync --------------------------------------------------------------------

Sync::Sync(QObject* parent) : QObject(parent), status_(new Status(this)) {
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/atrium/ClipSync"), status_,
                                                 QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals);

    connect(&clipboard_, &WaylandClipboard::copied, this, &Sync::copied);

    Compositor* compositor = Compositor::instance();
    const auto follow = [this, compositor] { setEnabled(compositor->setting(kSetting, false).toBool()); };
    connect(compositor, &Compositor::settingsChanged, this, follow);
    follow();

    Bluetooth* bt = Bluetooth::instance();
    const auto watch = [this, bt] {
        if (BluetoothAdapter* a = bt->adapter())
            connect(a, &BluetoothAdapter::devicesChanged, this, &Sync::devicesChanged, Qt::UniqueConnection);
        devicesChanged();
    };
    connect(bt, &Bluetooth::adapterChanged, this, watch);
    watch();
}

Sync::~Sync() = default;

bool Sync::isPhone(const BluetoothDevice* d) const {
    return d->paired() && d->icon().contains(QLatin1String("phone"));
}

void Sync::setEnabled(bool on) {
    if (on == enabled_)
        return;
    enabled_ = on;
    qCInfo(lc) << (on ? "on" : "off");
    if (on) {
        if (!history_)
            history_ = std::make_unique<RecentClips>();
        // Copied while this was off.
        if (clipboard_.last())
            history_->current(*clipboard_.last());
    } else {
        for (Link* l : std::as_const(links_))
            l->deleteLater();
        links_.clear();
        qDeleteAll(retries_);
        retries_.clear();
        attempts_.clear();
    }
    devicesChanged();
}

void Sync::devicesChanged() {
    BluetoothAdapter* a = Bluetooth::instance()->adapter();
    if (a && enabled_) {
        for (QObject* o : a->devices()) {
            auto* d = static_cast<BluetoothDevice*>(o);
            const QString path = d->path();
            if (d->connected() && isPhone(d) && !links_.contains(path) && !retries_.contains(path) &&
                !connecting_.value(path))
                schedule(d, 1'000);  // after its own connecting is done
            if (!d->connected()) {
                delete retries_.take(path);
                attempts_.remove(path);
            }
        }
    }
    updateStatus();
}

void Sync::schedule(BluetoothDevice* d, int ms) {
    auto* t = new QTimer(this);
    t->setSingleShot(true);
    const QString path = d->path();
    QPointer<BluetoothDevice> device(d);
    connect(t, &QTimer::timeout, this, [this, path, device] {
        if (QTimer* self = retries_.take(path))
            self->deleteLater();  // this is its own timeout
        if (device)
            tryConnect(device);
    });
    delete retries_.take(path);
    retries_.insert(path, t);
    t->start(ms);
}

void Sync::tryConnect(BluetoothDevice* d) {
    const QString path = d->path();
    if (!enabled_ || !d->connected() || links_.contains(path) || connecting_.value(path))
        return;
    connecting_[path] = true;
    updateStatus();
    // The lookup and connect block (the phone answers over the air).
    QPointer<Sync> self(this);
    QPointer<BluetoothDevice> device(d);
    std::thread([self, device, path, address = d->address().toStdString()] {
        std::string error;
        const int fd = connectService(address, kServiceUuid, error);
        QMetaObject::invokeMethod(qApp, [self, device, path, fd, error] {
            if (!self) {
                if (fd >= 0)
                    ::close(fd);
                return;
            }
            self->connecting_.remove(path);
            if (fd >= 0) {
                self->connected(path, fd);
                return;
            }
            const int n = self->attempts_.value(path);
            self->attempts_[path] = n + 1;
            qCInfo(lc) << "no clipboard module answered on" << (device ? device->name() : path) << "-"
                       << error.c_str();
            if (device && device->connected() && self->enabled_)
                self->schedule(device, kRetry[std::min<int>(n, std::size(kRetry) - 1)]);
            self->updateStatus();
        }, Qt::QueuedConnection);
    }).detach();
}

void Sync::connected(const QString& device, int fd) {
    if (!enabled_) {
        ::close(fd);
        return;
    }
    if (Link* old = links_.take(device))
        old->deleteLater();
    auto* link = new Link(*this, device, QSysInfo::machineHostName(), history_->recent(), fd);
    links_.insert(device, link);
    attempts_.remove(device);
    connect(link, &Link::closed, this, [this, link] {
        const QString path = link->device();
        if (links_.value(path) == link)
            links_.remove(path);
        link->deleteLater();
        qCInfo(lc) << "phone link closed";
        // Still connected (the module restarted?): try again.
        devicesChanged();
    });
    link->apply(link->session().start());
    updateStatus();
}

void Sync::copied(const Clip& c) {
    if (!enabled_)
        return;
    history_->current(c);
    if (c.time == 0)  // was there before we started: not a copy
        return;
    for (Link* l : std::as_const(links_))
        l->apply(l->session().copied(c));
}

void Sync::setClipboard(Link* from, const Clip& c) {
    clipboard_.set(c);
    history_->current(c);
    // Other phones get it as a copy made here.
    for (Link* l : std::as_const(links_))
        if (l != from)
            l->apply(l->session().copied(c));
}

void Sync::addHistory(const Clip& c) {
    history_->older(c);
    history_->toCliphist(c);
    addedHistory_ = true;
}

void Sync::merged(bool tookClipboard) {
    // What the phone added went on top of the picker's list; the clipboard
    // is still the newest.
    if (std::exchange(addedHistory_, false) && !tookClipboard && !history_->recent().empty())
        history_->toCliphist(history_->recent().front());
    updateStatus();
}

void Sync::updateStatus() {
    if (!enabled_) {
        status_->set(QStringLiteral("off"), {});
        return;
    }
    BluetoothAdapter* a = Bluetooth::instance()->adapter();
    if (!clipboard_.ok() || !a || !a->enabled()) {
        status_->set(QStringLiteral("unavailable"), {});
        return;
    }
    QString phone, state = QStringLiteral("waiting");
    for (QObject* o : a->devices()) {
        auto* d = static_cast<BluetoothDevice*>(o);
        if (!d->connected() || !isPhone(d))
            continue;
        phone = d->name();
        if (Link* l = links_.value(d->path()); l && l->session().synced()) {
            state = QStringLiteral("connected");
            break;
        }
        // Tried and nothing answered: its module is missing (or starting).
        state = attempts_.value(d->path()) >= 2 ? QStringLiteral("missing") : QStringLiteral("connecting");
    }
    status_->set(state, phone);
}

} // namespace atrium::clipsync
