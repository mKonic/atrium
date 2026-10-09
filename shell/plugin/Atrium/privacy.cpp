#include "privacy.hpp"

#include "compositor.hpp"

#include <QDir>
#include <QFile>
#include <QSocketNotifier>

#include <pulse/context.h>
#include <pulse/glib-mainloop.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace atrium {

namespace {

std::string prop(const pa_proplist* p, const char* key) {
    const char* v = p ? pa_proplist_gets(p, key) : nullptr;
    return v ? v : "";
}

const char* kindName(privacy::Kind k) {
    switch (k) {
    case privacy::Kind::Screen: return "screen";
    case privacy::Kind::Camera: return "camera";
    case privacy::Kind::Microphone: return "microphone";
    }
    return "";
}

} // namespace

Privacy* Privacy::instance() {
    static auto* self = new Privacy;
    return self;
}

Privacy::Privacy() {
    loop_ = pa_glib_mainloop_new(nullptr);
    retry_.setSingleShot(true);
    retry_.setInterval(2000);
    connect(&retry_, &QTimer::timeout, this, &Privacy::connectToServer);
    // Streams come and go in bursts (a call starting): look once they settle.
    soundSettle_.setSingleShot(true);
    soundSettle_.setInterval(150);
    connect(&soundSettle_, &QTimer::timeout, this, &Privacy::refreshSound);
    cameraSettle_.setSingleShot(true);
    cameraSettle_.setInterval(300);
    connect(&cameraSettle_, &QTimer::timeout, this, &Privacy::scanCameras);
    connect(Compositor::instance(), &Compositor::castsChanged, this, &Privacy::update);
    connectToServer();
    watchCameras();
}

bool Privacy::has(privacy::Kind k) const {
    return std::any_of(uses_.begin(), uses_.end(), [k](const privacy::Use& u) { return u.kind == k; });
}

QVariantList Privacy::uses() const {
    QVariantList out;
    for (const privacy::Use& u : uses_)
        out.append(QVariantMap{
            {"kind", kindName(u.kind)},
            {"app", QString::fromStdString(u.app)},
            // The menu's line for it.
            {"icon", u.kind == privacy::Kind::Screen ? "screen_record" : u.kind == privacy::Kind::Camera ? "videocam" : "mic"},
            {"text", QString::fromStdString(u.app) + (u.kind == privacy::Kind::Screen ? " is sharing the screen"
                                                      : u.kind == privacy::Kind::Camera ? " is using the camera"
                                                                                        : " is using the microphone")},
        });
    return out;
}

void Privacy::update() {
    std::vector<privacy::Use> all;
    for (const QVariant& v : Compositor::instance()->casts())
        all.push_back({privacy::Kind::Screen, v.toString().isEmpty() ? "An app" : v.toString().toStdString()});
    for (const std::string& a : cameras_)
        all.push_back({privacy::Kind::Camera, a});
    for (const std::string& a : microphones_)
        all.push_back({privacy::Kind::Microphone, a});
    all = privacy::merge(std::move(all));
    if (all != uses_) {
        uses_ = std::move(all);
        emit changed();
    }
}

// --- microphones ---------------------------------------------------------------------

void Privacy::connectToServer() {
    if (context_) {
        pa_context_disconnect(context_);
        pa_context_unref(context_);
    }
    pa_proplist* p = pa_proplist_new();
    pa_proplist_sets(p, PA_PROP_APPLICATION_NAME, "atrium");
    pa_proplist_sets(p, PA_PROP_APPLICATION_ID, "atrium-shell");
    context_ = pa_context_new_with_proplist(pa_glib_mainloop_get_api(loop_), "atrium-privacy", p);
    pa_proplist_free(p);
    pa_context_set_state_callback(context_, [](pa_context*, void* self) { static_cast<Privacy*>(self)->onState(); }, this);
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0)
        retry_.start();
}

void Privacy::onState() {
    const pa_context_state_t s = pa_context_get_state(context_);
    if (s == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(context_, [](pa_context*, pa_subscription_event_type_t, uint32_t, void* self) {
            static_cast<Privacy*>(self)->soundSettle_.start();
        }, this);
        const auto mask = pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT);
        if (pa_operation* op = pa_context_subscribe(context_, mask, nullptr, nullptr))
            pa_operation_unref(op);
        refreshSound();
    } else if (s == PA_CONTEXT_FAILED || s == PA_CONTEXT_TERMINATED) {
        microphones_.clear();
        update();
        retry_.start();
    }
}

void Privacy::refreshSound() {
    if (!context_ || pa_context_get_state(context_) != PA_CONTEXT_READY)
        return;
    if (listing_) {
        soundSettle_.start();  // after the listing under way
        return;
    }
    listing_ = true;
    monitors_.clear();
    pendingStreams_.clear();
    // Sources first (which are monitors), then the streams on them.
    if (pa_operation* op = pa_context_get_source_info_list(context_, &Privacy::sourceCb, this))
        pa_operation_unref(op);
}

void Privacy::sourceCb(pa_context* c, const pa_source_info* i, int eol, void* data) {
    auto* self = static_cast<Privacy*>(data);
    if (!eol && i) {
        self->monitors_[i->index] = i->monitor_of_sink != PA_INVALID_INDEX;
        return;
    }
    if (pa_operation* op = pa_context_get_source_output_info_list(c, &Privacy::outputCb, self))
        pa_operation_unref(op);
    else
        self->listing_ = false;
}

void Privacy::outputCb(pa_context*, const pa_source_output_info* i, int eol, void* data) {
    auto* self = static_cast<Privacy*>(data);
    if (!eol && i) {
        Stream s;
        s.source = i->source;
        s.capture.app = prop(i->proplist, PA_PROP_APPLICATION_NAME);
        s.capture.media_name = prop(i->proplist, PA_PROP_MEDIA_NAME);
        s.capture.corked = i->corked;
        s.capture.monitor = self->monitors_.value(i->source, false);
        self->pendingStreams_[i->index] = s;
        return;
    }
    self->listing_ = false;
    self->streams_ = self->pendingStreams_;
    std::vector<privacy::Capture> captures;
    for (const Stream& s : self->streams_)
        captures.push_back(s.capture);
    self->microphones_ = privacy::microphone_apps(captures);
    self->update();
}

// --- cameras ------------------------------------------------------------------------------

void Privacy::watchCameras() {
    inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_ < 0)
        return;
    // New cameras plugged in, and each one opened or closed.
    inotify_add_watch(inotify_, "/dev", IN_CREATE | IN_DELETE);
    auto watchDevices = [this] {
        for (const QString& name : QDir("/dev").entryList({"video*"}, QDir::System))
            if (privacy::is_camera(("/dev/" + name).toStdString()))
                inotify_add_watch(inotify_, ("/dev/" + name).toLocal8Bit().constData(), IN_OPEN | IN_CLOSE);
    };
    watchDevices();
    notifier_ = new QSocketNotifier(inotify_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this, watchDevices] {
        alignas(inotify_event) char buf[4096];
        bool created = false;
        ssize_t n;
        while ((n = read(inotify_, buf, sizeof buf)) > 0)
            for (char* p = buf; p < buf + n;) {
                auto* e = reinterpret_cast<inotify_event*>(p);
                if ((e->mask & IN_CREATE) && e->len && std::string_view(e->name).starts_with("video"))
                    created = true;
                p += sizeof(inotify_event) + e->len;
            }
        if (created)
            watchDevices();
        cameraSettle_.start();
    });
    scanCameras();
}

void Privacy::scanCameras() {
    // Who has a camera open: each process's open files.
    std::map<int, std::vector<std::string>> fds;
    std::map<int, std::string> names;
    for (const QString& pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const int id = pid.toInt(&ok);
        if (!ok)
            continue;
        const QString dir = "/proc/" + pid + "/fd";
        std::vector<std::string> targets;
        for (const QString& fd : QDir(dir).entryList(QDir::System | QDir::NoDotAndDotDot)) {
            const QString t = QFile::symLinkTarget(dir + "/" + fd);
            if (t.startsWith("/dev/video"))
                targets.push_back(t.toStdString());
        }
        if (targets.empty())
            continue;
        fds[id] = targets;
        QFile comm("/proc/" + pid + "/comm");
        if (comm.open(QIODevice::ReadOnly))
            names[id] = QString::fromUtf8(comm.readAll()).trimmed().toStdString();
    }
    cameras_ = privacy::camera_apps(fds, names);
    update();
}

} // namespace atrium
