#include "cast_stream.hpp"

#include "wayland_link.hpp"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

#include <QCoreApplication>
#include <QSocketNotifier>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/dynamic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include <cstring>

namespace atrium {

namespace {

constexpr int kBuffers = 4, kMinBuffers = 2;  // XDPW_PWR_BUFFERS(_MIN)
constexpr int kDamageSlots = 16;              // DAMAGE_REGION_COUNT

pw_loop* loop_ = nullptr;

// PipeWire's loop, run from Qt's: one connection for every stream.
pw_core* pipewire() {
    static pw_core* core = [] () -> pw_core* {
        pw_loop* loop = loop_ = pw_loop_new(nullptr);
        if (!loop)
            return nullptr;
        pw_context* context = pw_context_new(loop, nullptr, 0);
        pw_core* c = context ? pw_context_connect(context, nullptr, 0) : nullptr;
        if (!c)
            return nullptr;
        pw_loop_enter(loop);
        auto* notifier = new QSocketNotifier(pw_loop_get_fd(loop), QSocketNotifier::Read, QCoreApplication::instance());
        QObject::connect(notifier, &QSocketNotifier::activated, notifier, [loop] { pw_loop_iterate(loop, 0); });
        // PipeWire restarted: the streams are gone with it, and D-Bus starts a
        // fresh portal on the next request (as xdg-desktop-portal-wlr exits).
        static const pw_core_events events = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.error = [](void*, uint32_t id, int, int, const char*) {
                if (id == PW_ID_CORE)
                    QCoreApplication::exit(1);
            };
            return e;
        }();
        static spa_hook listener{};
        pw_core_add_listener(c, &listener, &events, nullptr);
        return c;
    }();
    return core;
}

// --- capture session ---------------------------------------------------------

const ext_image_copy_capture_session_v1_listener kSession = {
    .buffer_size = [](void* data, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto* s = static_cast<CastStream*>(data);
        s->pending_.width = int(w);
        s->pending_.height = int(h);
    },
    .shm_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
        auto* s = static_cast<CastStream*>(data);
        const uint32_t fourcc = cast::drm_from_shm(format);
        if (cast::bytes_per_pixel(fourcc) <= 0)
            return;
        for (const auto& f : s->pending_.shm)
            if (f.first == fourcc)
                return;
        s->pending_.shm.emplace_back(fourcc, 0);
    },
    .dmabuf_device = [](void* data, ext_image_copy_capture_session_v1*, wl_array* device) {
        if (device->size == sizeof(dev_t))
            std::memcpy(&static_cast<CastStream*>(data)->pending_.device, device->data, sizeof(dev_t));
    },
    .dmabuf_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t format, wl_array* modifiers) {
        auto* s = static_cast<CastStream*>(data);
        const auto* mods = static_cast<const uint64_t*>(modifiers->data);
        for (size_t i = 0; i < modifiers->size / sizeof(uint64_t); i++) {
            const std::pair<uint32_t, uint64_t> pair{format, mods[i]};
            if (std::find(s->pending_.dmabuf.begin(), s->pending_.dmabuf.end(), pair) == s->pending_.dmabuf.end())
                s->pending_.dmabuf.push_back(pair);
        }
    },
    .done = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<CastStream*>(data)->constraints_done(); },
    .stopped = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<CastStream*>(data)->session_stopped(); },
};

const ext_image_copy_capture_frame_v1_listener kFrame = {
    .transform = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t t) {
        static_cast<CastStream*>(data)->frame_transform(t);
    },
    .damage = [](void* data, ext_image_copy_capture_frame_v1*, int32_t x, int32_t y, int32_t w, int32_t h) {
        static_cast<CastStream*>(data)->frame_damage({x, y, w, h});
    },
    .presentation_time = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t hi, uint32_t lo, uint32_t nsec) {
        static_cast<CastStream*>(data)->frame_time((uint64_t(hi) << 32) | lo, nsec);
    },
    .ready = [](void* data, ext_image_copy_capture_frame_v1*) { static_cast<CastStream*>(data)->frame_ready(); },
    .failed = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t reason) {
        static_cast<CastStream*>(data)->frame_failed(reason);
    },
};

// --- PipeWire ----------------------------------------------------------------

const pw_stream_events kStream = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = [](void* data, pw_stream_state old, pw_stream_state state, const char*) {
        static_cast<CastStream*>(data)->stream_state(old, state);
    };
    e.param_changed = [](void* data, uint32_t id, const spa_pod* param) {
        static_cast<CastStream*>(data)->stream_param(id, param);
    };
    e.add_buffer = [](void* data, pw_buffer* b) { static_cast<CastStream*>(data)->stream_add_buffer(b); };
    e.remove_buffer = [](void* data, pw_buffer* b) { static_cast<CastStream*>(data)->stream_remove_buffer(b); };
    e.process = [](void* data) { static_cast<CastStream*>(data)->stream_process(); };
    return e;
}();

spa_pod* build_buffer(spa_pod_builder* b, uint32_t blocks, uint32_t datatype) {
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(kBuffers, kMinBuffers, 32), 0);
    spa_pod_builder_add(b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(int(blocks)), 0);
    spa_pod_builder_add(b, SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), 0);
    spa_pod_builder_add(b, SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(int(datatype)), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f));
}

// A format to offer: with modifiers to pick from (a dma-buf), one modifier
// chosen (fixated), or none (shared memory).
spa_pod* build_format(spa_pod_builder* b, spa_video_format format, uint32_t width, uint32_t height,
                      uint32_t framerate, const std::vector<uint64_t>& modifiers, bool fixated) {
    spa_pod_frame f[2];
    const spa_video_format opaque = cast::strip_alpha(format);
    spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    // Modifiers belong to their exact format, so no alpha-less twin then.
    if (!modifiers.empty() || opaque == SPA_VIDEO_FORMAT_UNKNOWN)
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    else
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, format, format, opaque), 0);
    if (fixated) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(b, int64_t(modifiers.front()));
    } else if (!modifiers.empty()) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(b, int64_t(modifiers.front()));  // the default
        for (uint64_t m : modifiers)
            spa_pod_builder_long(b, int64_t(m));
        spa_pod_builder_pop(b, &f[1]);
    }
    const spa_rectangle size = SPA_RECTANGLE(width, height);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
    // A variable frame rate, up to the screen's.
    const spa_fraction variable = SPA_FRACTION(0, 1), most = SPA_FRACTION(framerate, 1), least = SPA_FRACTION(1, 1);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable), 0);
    if (framerate > 0)
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&most, &least, &most), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &f[0]));
}

uint64_t object_serial(pw_stream* stream) {
    const pw_properties* props = pw_stream_get_properties(stream);
    const char* s = props ? pw_properties_get(props, PW_KEY_OBJECT_SERIAL) : nullptr;
    return s ? std::strtoull(s, nullptr, 10) : 0;
}

} // namespace

CastStream::CastStream(const Target& target, QObject* parent) : QObject(parent), target_(target) {
    pace_.setSingleShot(true);
    connect(&pace_, &QTimer::timeout, this, &CastStream::capture_now);
    retry_.setSingleShot(true);
    // PipeWire had no free buffer: ask again a frame later.
    connect(&retry_, &QTimer::timeout, this, [this] {
        if (stream_)
            pw_stream_trigger_process(stream_);
    });
}

CastStream::~CastStream() {
    stopping_ = true;
    if (stream_) {
        pw_stream_flush(stream_, false);
        pw_stream_disconnect(stream_);
        pw_stream_destroy(stream_);  // removes the buffers
    }
    if (frame_)
        ext_image_copy_capture_frame_v1_destroy(frame_);
    if (session_)
        ext_image_copy_capture_session_v1_destroy(session_);
    if (source_)
        ext_image_capture_source_v1_destroy(source_);
    if (gbm_) {
        const int fd = gbm_device_get_fd(gbm_);
        gbm_device_destroy(gbm_);
        close(fd);
    }
}

bool CastStream::start() {
    WaylandLink* link = WaylandLink::instance();
    if (!link || !link->copy_manager || !pipewire()) {
        qWarning("screencast: no Wayland capture or no PipeWire");
        return false;
    }
    if (target_.type == cast::Window) {
        WaylandLink::Toplevel* t = link->toplevel(target_.toplevel);
        if (!t || !link->toplevel_sources) {
            qWarning("screencast: window %s is gone", qPrintable(target_.toplevel));
            return false;
        }
        source_ = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(link->toplevel_sources, t->handle);
        framerate_ = 0;
        for (const auto& o : link->outputs())
            framerate_ = std::max(framerate_, uint32_t((o->refresh_mhz + 500) / 1000));
    } else {
        WaylandLink::Output* o = link->output(target_.output);
        if (!o || !link->output_sources) {
            qWarning("screencast: screen %s is gone", qPrintable(target_.output));
            return false;
        }
        source_ = ext_output_image_capture_source_manager_v1_create_source(link->output_sources, o->wl);
        framerate_ = uint32_t((o->refresh_mhz + 500) / 1000);
    }
    session_ = ext_image_copy_capture_manager_v1_create_session(
        link->copy_manager, source_, target_.cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(session_, &kSession, this);
    link->roundtrip();  // the buffer constraints
    if (current_.width <= 0 || current_.height <= 0) {
        qWarning("screencast: the capture session gave no buffer size");
        return false;
    }

    stream_ = pw_stream_new(pipewire(), "atrium-screencast",
                            pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", nullptr));
    if (!stream_)
        return false;
    pw_stream_add_listener(stream_, &stream_listener_, &kStream, this);
    uint8_t storage[4096];
    spa_pod_dynamic_builder b;
    spa_pod_dynamic_builder_init(&b, storage, sizeof storage, 4096);
    std::vector<const spa_pod*> params = formats(&b.b);
    pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      pw_stream_flags(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS), params.data(),
                      uint32_t(params.size()));
    spa_pod_dynamic_builder_clean(&b);
    return true;
}

bool CastStream::wait_ready(int ms) {
    // Start answers with the node, which PipeWire numbers a moment after.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!announced_ && !stopping_ && loop_ && std::chrono::steady_clock::now() < until)
        pw_loop_iterate(loop_, 10);
    return announced_ && !stopping_;
}

// --- capture -----------------------------------------------------------------

void CastStream::constraints_done() {
    for (auto& [fourcc, stride] : pending_.shm)
        stride = cast::bytes_per_pixel(fourcc) * pending_.width;
    if (pending_ == current_) {
        pending_ = {};
        return;
    }
    current_ = std::move(pending_);
    pending_ = {};
    update_gbm();
    update_params();
}

void CastStream::update_gbm() {
    drmDevice* dev = nullptr;
    if (!current_.device || drmGetDeviceFromDevId(current_.device, 0, &dev) != 0)
        return;
    drmDevice* old = nullptr;
    const bool same = gbm_ && drmGetDevice(gbm_device_get_fd(gbm_), &old) == 0 && drmDevicesEqual(dev, old);
    drmFreeDevice(&old);
    if (!same && (dev->available_nodes & (1 << DRM_NODE_RENDER))) {
        if (gbm_) {
            const int fd = gbm_device_get_fd(gbm_);
            gbm_device_destroy(gbm_);
            close(fd);
            gbm_ = nullptr;
        }
        const int fd = open(dev->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            gbm_ = gbm_create_device(fd);
            if (!gbm_)
                close(fd);
        }
    }
    drmFreeDevice(&dev);
}

std::vector<const spa_pod*> CastStream::formats(spa_pod_builder* b) {
    std::vector<const spa_pod*> out;
    if (!avoid_dmabuf_ && gbm_) {
        std::vector<uint32_t> seen;
        for (const auto& [fourcc, modifier] : current_.dmabuf) {
            const spa_video_format pw = cast::pw_from_drm(fourcc);
            if (pw == SPA_VIDEO_FORMAT_UNKNOWN || std::find(seen.begin(), seen.end(), fourcc) != seen.end())
                continue;
            seen.push_back(fourcc);
            std::vector<uint64_t> mods;
            for (const auto& [f, m] : current_.dmabuf)
                if (f == fourcc && (m == DRM_FORMAT_MOD_INVALID || gbm_device_get_format_modifier_plane_count(gbm_, f, m) > 0))
                    mods.push_back(m);
            if (!mods.empty())
                out.push_back(build_format(b, pw, uint32_t(current_.width), uint32_t(current_.height), framerate_, mods, false));
        }
    }
    for (const auto& [fourcc, stride] : current_.shm) {
        const spa_video_format pw = cast::pw_from_drm(fourcc);
        if (pw != SPA_VIDEO_FORMAT_UNKNOWN)
            out.push_back(build_format(b, pw, uint32_t(current_.width), uint32_t(current_.height), framerate_, {}, false));
    }
    return out;
}

void CastStream::update_params() {
    if (!stream_)
        return;
    uint8_t storage[4096];
    spa_pod_dynamic_builder b;
    spa_pod_dynamic_builder_init(&b, storage, sizeof storage, 4096);
    std::vector<const spa_pod*> params = formats(&b.b);
    pw_stream_update_params(stream_, params.data(), uint32_t(params.size()));
    spa_pod_dynamic_builder_clean(&b);
}

void CastStream::session_stopped() {
    // The window closed or the screen went: the app is told the session ended.
    stop();
}

void CastStream::stop() {
    if (stopping_)
        return;
    stopping_ = true;
    pace_.stop();
    retry_.stop();
    QMetaObject::invokeMethod(this, &CastStream::stopped, Qt::QueuedConnection);
}

void CastStream::frame_transform(uint32_t t) {
    transform_ = t;
}

void CastStream::frame_damage(const cast::Rect& r) {
    // Every other buffer now misses this part too.
    for (auto& b : buffers_)
        if (b.get() != buffer_)
            b->damage.push_back(r);
    damage_.push_back(r);
}

void CastStream::frame_time(uint64_t sec, uint32_t nsec) {
    pts_ns_ = sec * SPA_NSEC_PER_SEC + nsec;
}

void CastStream::frame_ready() {
    if (frame_) {
        ext_image_copy_capture_frame_v1_destroy(frame_);
        frame_ = nullptr;
    }
    completed_ = true;
    if (buffer_)
        buffer_->damage.clear();
    enqueue();
    damage_.clear();
    if (stream_)
        pw_stream_trigger_process(stream_);
}

void CastStream::frame_failed(uint32_t reason) {
    if (frame_) {
        ext_image_copy_capture_frame_v1_destroy(frame_);
        frame_ = nullptr;
    }
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        // New constraints are on their way: hand the buffer back as corrupt.
        enqueue();
        return;
    }
    stop();
}

bool CastStream::dequeue() {
    if (current_pw_)
        return true;
    current_pw_ = pw_stream_dequeue_buffer(stream_);
    if (!current_pw_)
        return false;
    buffer_ = static_cast<Buffer*>(current_pw_->user_data);
    completed_ = false;
    return true;
}

void CastStream::enqueue() {
    if (!current_pw_ || !stream_) {
        current_pw_ = nullptr;
        buffer_ = nullptr;
        return;
    }
    spa_buffer* buf = current_pw_->buffer;
    const bool corrupt = !completed_;
    if (auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header)))) {
        h->pts = int64_t(pts_ns_);
        h->flags = corrupt ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
        h->seq = seq_++;
        h->dts_offset = 0;
    }
    if (auto* vt = static_cast<spa_meta_videotransform*>(
            spa_buffer_find_meta_data(buf, SPA_META_VideoTransform, sizeof(spa_meta_videotransform))))
        vt->transform = transform_;
    if (spa_meta* meta = spa_buffer_find_meta(buf, SPA_META_VideoDamage)) {
        const size_t count = meta->size / sizeof(spa_region);
        const std::vector<cast::Rect> fitted = cast::fit_damage(damage_, count);
        auto* region = static_cast<spa_region*>(meta->data);
        for (size_t i = 0; i < count; i++)
            region[i] = i < fitted.size()
                ? SPA_REGION(fitted[i].x, fitted[i].y, uint32_t(fitted[i].width), uint32_t(fitted[i].height))
                : SPA_REGION(0, 0, 0, 0);
    }
    for (uint32_t i = 0; i < buf->n_datas; i++)
        buf->datas[i].chunk->flags = corrupt ? SPA_CHUNK_FLAG_CORRUPTED : SPA_CHUNK_FLAG_NONE;
    pw_stream_queue_buffer(stream_, current_pw_);
    current_pw_ = nullptr;
    buffer_ = nullptr;
}

void CastStream::capture() {
    using namespace std::chrono;
    const int64_t elapsed = duration_cast<nanoseconds>(steady_clock::now() - last_start_).count();
    const uint64_t delay = cast::frame_delay_ns(framerate_, elapsed);
    if (delay > 0)
        pace_.start(int((delay + 999999) / 1000000));
    else
        capture_now();
}

void CastStream::capture_now() {
    if (stopping_ || !session_ || !buffer_ || frame_)
        return;
    last_start_ = std::chrono::steady_clock::now();
    frame_ = ext_image_copy_capture_session_v1_create_frame(session_);
    ext_image_copy_capture_frame_v1_add_listener(frame_, &kFrame, this);
    ext_image_copy_capture_frame_v1_attach_buffer(frame_, buffer_->wl);
    for (const cast::Rect& r : buffer_->damage)
        ext_image_copy_capture_frame_v1_damage_buffer(frame_, r.x, r.y, r.width, r.height);
    ext_image_copy_capture_frame_v1_capture(frame_);
}

// --- the stream ----------------------------------------------------------------

void CastStream::stream_state(int, int state) {
    node_ = pw_stream_get_node_id(stream_);
    serial_ = object_serial(stream_);
    if (!announced_ && node_ != SPA_ID_INVALID) {
        announced_ = true;
        emit ready();
    }
    streaming_ = state == PW_STREAM_STATE_STREAMING;
    if (streaming_) {
        if (dequeue())
            capture();
    } else {
        pace_.stop();
        retry_.stop();
        if (state == PW_STREAM_STATE_PAUSED && current_pw_ && !frame_)
            enqueue();
        if (state == PW_STREAM_STATE_ERROR)
            stop();
    }
}

void CastStream::stream_param(uint32_t id, const spa_pod* param) {
    if (!param || id != SPA_PARAM_Format)
        return;
    spa_format_video_raw_parse(param, &format_);
    if (format_.max_framerate.denom > 0 && format_.max_framerate.num > 0)
        framerate_ = format_.max_framerate.num / format_.max_framerate.denom;

    uint8_t storage[4096];
    spa_pod_dynamic_builder b;
    spa_pod_dynamic_builder_init(&b, storage, sizeof storage, 4096);
    std::vector<const spa_pod*> params;
    uint32_t blocks = 1, datatype = 1u << SPA_DATA_MemFd;
    const uint32_t fourcc = cast::drm_from_pw(format_.format);

    if (const spa_pod_prop* prop = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier)) {
        use_dmabuf_ = true;
        datatype = 1u << SPA_DATA_DmaBuf;
        if (prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) {
            // The consumer left the modifier open: pick one gbm can allocate.
            const spa_pod* choice = &prop->value;
            const uint32_t n = SPA_POD_CHOICE_N_VALUES(choice) - 1;
            const auto* mods = static_cast<const uint64_t*>(SPA_POD_CHOICE_VALUES(choice)) + 1;
            std::optional<uint64_t> chosen;
            if (gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm_, uint32_t(current_.width), uint32_t(current_.height),
                                                           fourcc, mods, n, GBM_BO_USE_RENDERING)) {
                chosen = gbm_bo_get_modifier(bo);
                gbm_bo_destroy(bo);
            }
            for (uint32_t i = 0; !chosen && i < n; i++) {
                if (mods[i] != DRM_FORMAT_MOD_INVALID && mods[i] != DRM_FORMAT_MOD_LINEAR)
                    continue;
                const uint32_t flags = GBM_BO_USE_RENDERING | (mods[i] == DRM_FORMAT_MOD_LINEAR ? GBM_BO_USE_LINEAR : 0);
                if (gbm_bo* bo = gbm_bo_create(gbm_, uint32_t(current_.width), uint32_t(current_.height), fourcc, flags)) {
                    chosen = gbm_bo_get_modifier(bo);
                    gbm_bo_destroy(bo);
                }
            }
            if (!chosen)
                avoid_dmabuf_ = true;  // shared memory it is
            else
                params.push_back(build_format(&b.b, format_.format, uint32_t(current_.width), uint32_t(current_.height),
                                              framerate_, {*chosen}, true));
            for (const spa_pod* p : formats(&b.b))
                params.push_back(p);
            pw_stream_update_params(stream_, params.data(), uint32_t(params.size()));
            spa_pod_dynamic_builder_clean(&b);
            return;
        }
        if (format_.modifier != DRM_FORMAT_MOD_INVALID && gbm_) {
            const int planes = gbm_device_get_format_modifier_plane_count(gbm_, fourcc, format_.modifier);
            if (planes > 0)
                blocks = uint32_t(planes);
        }
    } else {
        use_dmabuf_ = false;
    }

    params.push_back(build_buffer(&b.b, blocks, datatype));
    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&b.b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))));
    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&b.b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform), SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(spa_meta_videotransform)))));
    params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&b.b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(int(sizeof(spa_region) * kDamageSlots), int(sizeof(spa_region)),
                                 int(sizeof(spa_region) * kDamageSlots)))));
    pw_stream_update_params(stream_, params.data(), uint32_t(params.size()));
    spa_pod_dynamic_builder_clean(&b);
}

std::unique_ptr<CastStream::Buffer> CastStream::make_buffer() {
    auto buf = std::make_unique<Buffer>();
    WaylandLink* link = WaylandLink::instance();
    const uint32_t fourcc = cast::drm_from_pw(format_.format);
    const uint32_t w = uint32_t(current_.width), h = uint32_t(current_.height);
    buf->dmabuf = use_dmabuf_;
    // A new buffer holds nothing yet.
    buf->damage.push_back({0, 0, current_.width, current_.height});
    if (!use_dmabuf_) {
        int stride = 0;
        for (const auto& [f, s] : current_.shm)
            if (f == fourcc)
                stride = s;
        if (!stride || !link->shm)
            return nullptr;
        buf->planes = 1;
        buf->stride[0] = uint32_t(stride);
        buf->size[0] = uint32_t(stride) * h;
        buf->fd[0] = memfd_create("atrium-screencast", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (buf->fd[0] < 0 || ftruncate(buf->fd[0], buf->size[0]) < 0)
            return nullptr;
        wl_shm_pool* pool = wl_shm_create_pool(link->shm, buf->fd[0], int32_t(buf->size[0]));
        buf->wl = wl_shm_pool_create_buffer(pool, 0, int32_t(w), int32_t(h), stride, cast::shm_from_drm(fourcc));
        wl_shm_pool_destroy(pool);
        return buf;
    }
    if (!gbm_ || !link->dmabuf)
        return nullptr;
    gbm_bo* bo = nullptr;
    if (format_.modifier != DRM_FORMAT_MOD_INVALID)
        bo = gbm_bo_create_with_modifiers2(gbm_, w, h, fourcc, &format_.modifier, 1, GBM_BO_USE_RENDERING);
    else
        bo = gbm_bo_create(gbm_, w, h, fourcc, GBM_BO_USE_RENDERING);
    if (!bo && format_.modifier == DRM_FORMAT_MOD_LINEAR)
        bo = gbm_bo_create(gbm_, w, h, fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo)
        return nullptr;
    buf->planes = std::min(gbm_bo_get_plane_count(bo), 4);
    const uint64_t modifier = gbm_bo_get_modifier(bo);
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(link->dmabuf);
    for (int p = 0; p < buf->planes; p++) {
        buf->stride[p] = gbm_bo_get_stride_for_plane(bo, p);
        buf->offset[p] = gbm_bo_get_offset(bo, p);
        buf->fd[p] = gbm_bo_get_fd_for_plane(bo, p);
        if (buf->fd[p] < 0) {
            zwp_linux_buffer_params_v1_destroy(params);
            gbm_bo_destroy(bo);
            return nullptr;
        }
        zwp_linux_buffer_params_v1_add(params, buf->fd[p], uint32_t(p), buf->offset[p], buf->stride[p],
                                       uint32_t(modifier >> 32), uint32_t(modifier & 0xffffffff));
    }
    buf->wl = zwp_linux_buffer_params_v1_create_immed(params, int32_t(w), int32_t(h), fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    gbm_bo_destroy(bo);
    return buf->wl ? std::move(buf) : nullptr;
}

void CastStream::stream_add_buffer(pw_buffer* b) {
    spa_data* d = b->buffer->datas;
    const bool memfd = d[0].type & (1u << SPA_DATA_MemFd);
    std::unique_ptr<Buffer> buf = make_buffer();
    if (!buf || uint32_t(buf->planes) != b->buffer->n_datas) {
        // Nothing to fill: let the app know rather than send blank frames.
        stop();
        return;
    }
    for (uint32_t p = 0; p < b->buffer->n_datas; p++) {
        d[p].type = memfd ? SPA_DATA_MemFd : SPA_DATA_DmaBuf;
        d[p].flags = SPA_DATA_FLAG_READABLE | (memfd ? SPA_DATA_FLAG_MAPPABLE : 0);
        d[p].fd = buf->fd[p];
        d[p].mapoffset = 0;
        d[p].maxsize = buf->size[p];
        d[p].data = nullptr;
        d[p].chunk->offset = buf->offset[p];
        d[p].chunk->stride = int32_t(buf->stride[p]);
        // Consumers check the size to tell a buffer is good, dma-bufs too.
        d[p].chunk->size = buf->dmabuf ? 9 : buf->size[p];
    }
    b->user_data = buf.get();
    buffers_.push_back(std::move(buf));
}

void CastStream::stream_remove_buffer(pw_buffer* b) {
    auto* buf = static_cast<Buffer*>(b->user_data);
    if (current_pw_ == b) {
        current_pw_ = nullptr;
        buffer_ = nullptr;
    }
    if (buf) {
        if (buf->wl)
            wl_buffer_destroy(buf->wl);
        for (int p = 0; p < buf->planes; p++)
            if (buf->fd[p] >= 0)
                close(buf->fd[p]);
        buffers_.remove_if([buf](const std::unique_ptr<Buffer>& x) { return x.get() == buf; });
    }
    for (uint32_t p = 0; p < b->buffer->n_datas; p++)
        b->buffer->datas[p].fd = -1;
    b->user_data = nullptr;
}

void CastStream::stream_process() {
    if (!streaming_ || current_pw_)
        return;
    if (!dequeue()) {
        retry_.start(framerate_ > 0 ? int(1000 / framerate_) : 16);
        return;
    }
    retry_.stop();
    capture();
}

} // namespace atrium
