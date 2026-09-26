#include "wayland_clipboard.hpp"

#include "ext-data-control-v1-client-protocol.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QDateTime>
#include <QLoggingCategory>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>

namespace atrium::clipsync {

namespace {

Q_LOGGING_CATEGORY(lc, "atrium.clipsync")

constexpr std::string_view kPasswordHint = "x-kde-passwordManagerHint";
constexpr std::string_view kPng = "image/png";
// What text goes by, best first. Setting text offers all of them.
constexpr std::string_view kTextMimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};

bool isText(std::string_view mime) {
    return std::ranges::find(kTextMimes, mime) != std::end(kTextMimes);
}

} // namespace

// A copy being read from its app, through a pipe.
struct WaylandClipboard::Read {
    int fd = -1;
    std::string mime;
    std::string data;
    std::unique_ptr<QSocketNotifier> notifier;
    ~Read() {
        if (fd >= 0)
            close(fd);
    }
};

// A clip going out to an app that pastes it.
struct WaylandClipboard::Write : QObject {
    int fd;
    std::string data;
    std::size_t done = 0;
    QSocketNotifier notifier;
    Write(int fd, std::string data) : fd(fd), data(std::move(data)), notifier(fd, QSocketNotifier::Write) {
        connect(&notifier, &QSocketNotifier::activated, this, [this] { pump(); });
    }
    ~Write() override { close(fd); }
    void pump() {
        while (done < data.size()) {
            const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
            if (n > 0) {
                done += std::size_t(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            break;  // the app went away
        }
        deleteLater();
    }
};

const wl_registry_listener WaylandClipboard::registryListener = {
    .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto* self = static_cast<WaylandClipboard*>(data);
        if (!std::strcmp(interface, wl_seat_interface.name) && !self->seat_)
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        else if (!std::strcmp(interface, ext_data_control_manager_v1_interface.name))
            self->manager_ = static_cast<ext_data_control_manager_v1*>(
                wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, std::min(version, 1u)));
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

const ext_data_control_offer_v1_listener WaylandClipboard::offerListener = {
    .offer = [](void* data, ext_data_control_offer_v1* offer, const char* mime) {
        auto* self = static_cast<WaylandClipboard*>(data);
        if (Offer* o = self->find(offer))
            o->mimes.emplace_back(mime);
    },
};

const ext_data_control_device_v1_listener WaylandClipboard::deviceListener = {
    .data_offer = [](void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
        auto* self = static_cast<WaylandClipboard*>(data);
        self->offers_.push_back({offer, {}});
        ext_data_control_offer_v1_add_listener(offer, &offerListener, self);
    },
    .selection = [](void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
        static_cast<WaylandClipboard*>(data)->selection(offer);
    },
    .finished = [](void* data, ext_data_control_device_v1*) {
        qCWarning(lc) << "the compositor ended clipboard access";
        auto* self = static_cast<WaylandClipboard*>(data);
        ext_data_control_device_v1_destroy(self->device_);
        self->device_ = nullptr;
    },
    .primary_selection = [](void* data, ext_data_control_device_v1*, ext_data_control_offer_v1* offer) {
        // The middle-click selection isn't synced.
        if (offer)
            static_cast<WaylandClipboard*>(data)->dropOffer(offer);
    },
};

const ext_data_control_source_v1_listener WaylandClipboard::sourceListener = {
    .send = [](void* data, ext_data_control_source_v1*, const char* mime, int32_t fd) {
        auto* self = static_cast<WaylandClipboard*>(data);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        const bool png = std::string_view(mime) == kPng && self->sourceClip_.mime != kPng;
        auto* w = new Write(fd, png ? self->sourcePng() : self->sourceClip_.data);
        w->setParent(self);
        w->pump();
    },
    .cancelled = [](void* data, ext_data_control_source_v1* source) {
        auto* self = static_cast<WaylandClipboard*>(data);
        if (self->source_ == source)
            self->source_ = nullptr;
        ext_data_control_source_v1_destroy(source);
    },
};

WaylandClipboard::WaylandClipboard(QObject* parent) : QObject(parent) {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        qCWarning(lc) << "no Wayland display";
        return;
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener, this);
    wl_display_roundtrip(display_);
    if (!seat_ || !manager_) {
        qCWarning(lc) << "the compositor has no" << (seat_ ? "ext-data-control-v1" : "seat");
        return;
    }
    device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
    ext_data_control_device_v1_add_listener(device_, &deviceListener, this);
    notifier_ = std::make_unique<QSocketNotifier>(wl_display_get_fd(display_), QSocketNotifier::Read);
    connect(notifier_.get(), &QSocketNotifier::activated, this, &WaylandClipboard::dispatch);
    wl_display_roundtrip(display_);
}

WaylandClipboard::~WaylandClipboard() {
    read_.reset();
    for (const Offer& o : offers_)
        ext_data_control_offer_v1_destroy(o.offer);
    if (source_)
        ext_data_control_source_v1_destroy(source_);
    if (device_)
        ext_data_control_device_v1_destroy(device_);
    if (manager_)
        ext_data_control_manager_v1_destroy(manager_);
    if (seat_)
        wl_seat_destroy(seat_);
    if (registry_)
        wl_registry_destroy(registry_);
    if (display_)
        wl_display_disconnect(display_);
}

void WaylandClipboard::dispatch() {
    if (wl_display_dispatch(display_) < 0) {
        qCWarning(lc) << "lost the Wayland connection";
        notifier_->setEnabled(false);
        QCoreApplication::exit(1);
        return;
    }
    flush();
}

void WaylandClipboard::flush() {
    wl_display_flush(display_);
}

WaylandClipboard::Offer* WaylandClipboard::find(ext_data_control_offer_v1* offer) {
    for (Offer& o : offers_)
        if (o.offer == offer)
            return &o;
    return nullptr;
}

void WaylandClipboard::dropOffer(ext_data_control_offer_v1* offer) {
    std::erase_if(offers_, [offer](const Offer& o) { return o.offer == offer; });
    ext_data_control_offer_v1_destroy(offer);
}

void WaylandClipboard::selection(ext_data_control_offer_v1* offer) {
    const bool first = std::exchange(first_, false);
    // Every offer but this one is done with.
    for (auto it = offers_.begin(); it != offers_.end();) {
        if (it->offer != offer) {
            ext_data_control_offer_v1_destroy(it->offer);
            it = offers_.erase(it);
        } else {
            ++it;
        }
    }
    read_.reset();
    Offer* o = offer ? find(offer) : nullptr;
    // Cleared, our own clip coming back, or a password.
    if (!o || source_ || std::ranges::contains(o->mimes, kPasswordHint))
        return;

    std::string mime;
    for (std::string_view want : kTextMimes)
        if (std::ranges::contains(o->mimes, want)) {
            mime = want;
            break;
        }
    if (mime.empty()) {
        for (const std::string& m : o->mimes)
            if (m == "image/png") {
                mime = m;
                break;
            }
        for (const std::string& m : o->mimes)
            if (mime.empty() && m.starts_with("image/"))
                mime = m;
    }
    if (mime.empty())
        return;

    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    ext_data_control_offer_v1_receive(offer, mime.c_str(), fds[1]);
    flush();
    close(fds[1]);

    auto r = std::make_unique<Read>();
    r->fd = fds[0];
    r->mime = isText(mime) ? std::string(kText) : mime;
    Read* raw = r.get();
    r->notifier = std::make_unique<QSocketNotifier>(fds[0], QSocketNotifier::Read);
    // A clip already there when we started has no time we know.
    const std::int64_t time = first ? 0 : QDateTime::currentMSecsSinceEpoch();
    connect(r->notifier.get(), &QSocketNotifier::activated, this, [this, raw, time] {
        char buf[65536];
        for (;;) {
            const ssize_t n = ::read(raw->fd, buf, sizeof buf);
            if (n > 0) {
                raw->data.append(buf, std::size_t(n));
                if (raw->data.size() > kMaxClip) {  // nothing will take it
                    read_.reset();
                    return;
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                return;
            break;
        }
        Clip c{raw->mime, std::move(raw->data), time};
        read_.reset();
        if (c.data.empty())
            return;
        last_ = c;
        emit copied(c);
    });
    read_ = std::move(r);
}

const std::string& WaylandClipboard::sourcePng() {
    if (!sourcePng_) {
        QByteArray out;
        QBuffer buffer(&out);
        buffer.open(QIODevice::WriteOnly);
        const QImage image = QImage::fromData(QByteArrayView(sourceClip_.data.data(), qsizetype(sourceClip_.data.size())));
        if (image.isNull() || !image.save(&buffer, "PNG"))
            qCWarning(lc) << "couldn't turn a" << sourceClip_.mime.c_str() << "picture into PNG";
        sourcePng_ = out.toStdString();
    }
    return *sourcePng_;
}

void WaylandClipboard::set(const Clip& clip) {
    if (!device_)
        return;
    auto* source = ext_data_control_manager_v1_create_data_source(manager_);
    ext_data_control_source_v1_add_listener(source, &sourceListener, this);
    if (clip.mime == kText) {
        for (std::string_view m : kTextMimes)
            ext_data_control_source_v1_offer(source, std::string(m).c_str());
    } else {
        ext_data_control_source_v1_offer(source, clip.mime.c_str());
        // Most apps paste pictures as PNG only (a phone's screenshots are
        // JPEG): converted when one asks.
        if (clip.mime != kPng)
            ext_data_control_source_v1_offer(source, std::string(kPng).c_str());
    }
    // The old source (ours too) is cancelled by this.
    source_ = source;
    sourceClip_ = clip;
    sourcePng_.reset();
    last_ = clip;
    ext_data_control_device_v1_set_selection(device_, source);
    flush();
}

} // namespace atrium::clipsync
