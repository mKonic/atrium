#include "screencast.hpp"

#include "output.hpp"
#include "render/renderer.hpp"
#include "screencast_core.hpp"
#include "server.hpp"
#include "util/log.hpp"
#include "view.hpp"

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <ctime>

namespace atrium {

namespace {

constexpr int kDamageRects = 16;

int64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

// One of PipeWire's buffers, as a Buffer copies go into.
struct Slot {
    Buffer* buffer = nullptr;  // a DMA-BUF from the allocator, or `memory` below
    // MemFd: the mapping copies write.
    Buffer memory_base{};
    void* map = nullptr;
    size_t size = 0, stride = 0;
    uint32_t format = 0;
    int fd = -1;

    static Slot* of_memory(Buffer* b) {
        return reinterpret_cast<Slot*>(reinterpret_cast<char*>(b) - offsetof(Slot, memory_base));
    }
};

const BufferImpl kMemoryImpl = {
    // Its last lock went after PipeWire let go of it: the slot goes too.
    .destroy = [](Buffer* b) {
        buffer_finish(b);
        Slot* s = Slot::of_memory(b);
        munmap(s->map, s->size);
        close(s->fd);
        delete s;
    },
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = [](Buffer* b, uint32_t, void** data, uint32_t* format, size_t* stride) {
        Slot* s = Slot::of_memory(b);
        *data = s->map;
        *format = s->format;
        *stride = s->stride;
        return true;
    },
    .end_data_ptr_access = [](Buffer*) {},
};

} // namespace

struct ScreenCast::CoreListener {
    spa_hook hook{};
    pw_core_events events{};
};

struct ScreenCast::Stream {
    ScreenCast* owner_ = nullptr;
    uint64_t id = 0, tag = 0;
    wl::Capture::Target target;
    std::function<void(const Ready&)> ready;
    std::function<void()> ended;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    pw_stream_events events{};
    wl_event_source* timer = nullptr;

    int width = 0, height = 0;        // what the copies are
    uint32_t drm_format = 0;           // of the buffers
    std::vector<uint64_t> modifiers;   // what we can make DMA-BUFs with (empty: none)
    spa_video_info_raw format{};       // negotiated
    std::optional<uint64_t> modifier;  // a DMA-BUF one, once picked
    uint32_t max_rate = 60;            // frames a second we offer at most

    pw_buffer* in_flight = nullptr;  // being copied into
    bool copying = false;
    bool streaming = false;
    bool first = true;
    bool closed = false;
    int64_t last_frame = 0;
    uint64_t seq = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    ~Stream() {
        *alive = false;
        if (timer)
            wl_event_source_remove(timer);
        if (stream)
            pw_stream_destroy(stream);  // removes its buffers
    }

    Server& server() { return owner_->server_; }

    bool constraints(int* w, int* h, uint32_t* drm, std::vector<uint64_t>* mods) {
        wl::Capture& cap = *server().wl->capture;
        const auto c = cap.constraints ? cap.constraints(target) : std::nullopt;
        if (!c)
            return false;
        *w = c->width;
        *h = c->height;
        // 8 bits a channel whatever the screen draws in (a 10-bit frame is
        // converted as it is copied): what every app takes.
        *drm = target.toplevel ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
        mods->clear();
        if (const FormatSet* set = server().renderer->texture_formats(BUFFER_CAP_DMABUF))
            if (const DrmFormat* f = set->get(*drm))
                *mods = f->modifiers;
        if (!server().allocator)
            mods->clear();
        return true;
    }

    // What we offer: DMA-BUFs with our modifiers (the app picks), then
    // memory. `fixed`: the modifier we picked from the app's, first.
    std::vector<const spa_pod*> formats(spa_pod_builder* b, std::optional<uint64_t> fixed) {
        std::vector<const spa_pod*> out;
        auto one = [&](const std::vector<uint64_t>* mods, uint32_t flags) {
            spa_pod_frame f[2];
            spa_rectangle size = SPA_RECTANGLE(uint32_t(width), uint32_t(height));
            spa_fraction zero = SPA_FRACTION(0, 1), most = SPA_FRACTION(max_rate, 1);
            const uint32_t main = screencast_spa_format(drm_format);
            spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
            spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
            spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
            if (main == SPA_VIDEO_FORMAT_BGRA)  // the same without alpha, too
                spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
                                    SPA_POD_CHOICE_ENUM_Id(3, main, main, SPA_VIDEO_FORMAT_BGRx), 0);
            else
                spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(main), 0);
            if (mods) {
                spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, flags);
                spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
                bool head = true;
                for (uint64_t m : *mods) {
                    spa_pod_builder_long(b, int64_t(m));
                    if (std::exchange(head, false))
                        spa_pod_builder_long(b, int64_t(m));  // the default, then the choices
                }
                spa_pod_builder_pop(b, &f[1]);
            }
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&zero), 0);
            spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
                                SPA_POD_CHOICE_RANGE_Fraction(&most, &zero, &most), 0);
            out.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f[0])));
        };
        if (!modifiers.empty()) {
            if (fixed) {
                const std::vector<uint64_t> just{*fixed};
                one(&just, SPA_POD_PROP_FLAG_MANDATORY);
            }
            one(&modifiers, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        }
        one(nullptr, 0);
        return out;
    }

    void update_formats(std::optional<uint64_t> fixed) {
        uint8_t buf[4096];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        auto params = formats(&b, fixed);
        pw_stream_update_params(stream, params.data(), uint32_t(params.size()));
    }

    // Buffers and metadata, once the format is settled.
    void buffer_params() {
        uint8_t buf[2048];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        std::vector<const spa_pod*> params;
        const int stride = width * 4;
        spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8), 0);
        if (modifier) {
            spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(planes_for(*modifier)),
                                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf), 0);
        } else {
            spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
                                SPA_POD_Int(stride * height), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
                                SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), SPA_PARAM_BUFFERS_dataType,
                                SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd), 0);
        }
        params.push_back(static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f)));
        params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))));
        params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
            SPA_PARAM_META_size,
            SPA_POD_CHOICE_RANGE_Int(int(sizeof(spa_meta_region)) * kDamageRects, int(sizeof(spa_meta_region)),
                                     int(sizeof(spa_meta_region)) * kDamageRects))));
        pw_stream_update_params(stream, params.data(), uint32_t(params.size()));
    }

    int planes_for(uint64_t mod) {
        // What a buffer with it has: one made to find out.
        Buffer* test = server().allocator->allocate(width, height, drm_format, {mod});
        if (!test)
            return 1;
        DmabufAttributes a{};
        const int n = buffer_get_dmabuf(test, &a) ? a.n_planes : 1;
        buffer_drop(test);
        return n;
    }

    // A modifier from the app's list we can make buffers with, and render
    // into (NVIDIA can't with some).
    std::optional<uint64_t> pick(const std::vector<uint64_t>& offered) {
        for (uint64_t m : screencast_modifiers(offered, modifiers)) {
            Buffer* test = server().allocator->allocate(width, height, drm_format, {m});
            if (!test)
                continue;
            DmabufAttributes a{};
            bool ok = buffer_get_dmabuf(test, &a);
            if (ok) {
                render::RenderPass* pass = server().renderer->begin_buffer_pass(test, nullptr);
                ok = pass && pass->submit();
            }
            buffer_drop(test);
            if (ok)
                return m;
        }
        return std::nullopt;
    }

    void on_state(pw_stream_state state, const char* error) {
        if (state == PW_STREAM_STATE_ERROR) {
            alog(Log::Error, "screencast %llu: %s", (unsigned long long)id, error ? error : "error");
            if (!ready)
                return close_stream();
        }
        if (state == PW_STREAM_STATE_PAUSED && ready) {
            Ready r{.node = pw_stream_get_node_id(stream), .width = width, .height = height};
            std::exchange(ready, {})(r);
        }
        if ((state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) && ready)
            std::exchange(ready, {})(Ready{.error = error ? error : "PipeWire refused the stream"});
        streaming = state == PW_STREAM_STATE_STREAMING;
        if (streaming) {
            first = true;
            pump();
        }
    }

    void on_param(uint32_t param, const spa_pod* pod) {
        if (param != SPA_PARAM_Format || !pod)
            return;
        if (spa_format_video_raw_parse(pod, &format) < 0)
            return;
        const spa_pod_prop* mod = spa_pod_find_prop(pod, nullptr, SPA_FORMAT_VIDEO_modifier);
        if (mod && !modifiers.empty()) {
            if (mod->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) {
                // The app says which it can take: we pick, it confirms.
                std::vector<uint64_t> offered;
                const uint32_t n = SPA_POD_CHOICE_N_VALUES(&mod->value);
                const auto* values = static_cast<const uint64_t*>(SPA_POD_CHOICE_VALUES(&mod->value));
                for (uint32_t i = 0; i < n; ++i)
                    offered.push_back(values[i]);
                const auto picked = pick(offered);
                if (!picked)
                    for (uint64_t m : offered)  // none works: never offer them again
                        std::erase(modifiers, m);
                update_formats(picked);
                return;
            }
            modifier = format.modifier;
        } else {
            modifier.reset();
        }
        buffer_params();
    }

    void on_add_buffer(pw_buffer* pwb) {
        spa_data* d = pwb->buffer->datas;
        auto slot = std::make_unique<Slot>();
        if (modifier && (d[0].type & (1 << SPA_DATA_DmaBuf))) {
            Buffer* b = server().allocator->allocate(width, height, drm_format, {*modifier});
            DmabufAttributes a{};
            if (!b || !buffer_get_dmabuf(b, &a) || a.n_planes != int(pwb->buffer->n_datas)) {
                if (b)
                    buffer_drop(b);
                return;
            }
            for (int i = 0; i < a.n_planes; ++i) {
                d[i].type = SPA_DATA_DmaBuf;
                d[i].flags = SPA_DATA_FLAG_READABLE;
                d[i].fd = a.fd[i];
                d[i].mapoffset = 0;
                d[i].maxsize = a.stride[i] * uint32_t(height);
                d[i].data = nullptr;
                d[i].chunk->offset = a.offset[i];
                d[i].chunk->stride = int32_t(a.stride[i]);
                d[i].chunk->size = d[i].maxsize;
            }
            slot->buffer = b;
        } else if (d[0].type & (1 << SPA_DATA_MemFd)) {
            slot->stride = size_t(width) * 4;
            slot->size = slot->stride * size_t(height);
            slot->format = drm_format;
            slot->fd = memfd_create("atrium-screencast", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (slot->fd < 0 || ftruncate(slot->fd, off_t(slot->size)) < 0)
                return;
            fcntl(slot->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
            slot->map = mmap(nullptr, slot->size, PROT_READ | PROT_WRITE, MAP_SHARED, slot->fd, 0);
            if (slot->map == MAP_FAILED) {
                slot->map = nullptr;
                close(slot->fd);
                return;
            }
            d[0].type = SPA_DATA_MemFd;
            d[0].flags = SPA_DATA_FLAG_READABLE;
            d[0].fd = slot->fd;
            d[0].mapoffset = 0;
            d[0].maxsize = uint32_t(slot->size);
            d[0].data = nullptr;
            d[0].chunk->offset = 0;
            d[0].chunk->stride = int32_t(slot->stride);
            d[0].chunk->size = uint32_t(slot->size);
            buffer_init(&slot->memory_base, &kMemoryImpl, width, height);
            slot->buffer = &slot->memory_base;
        } else {
            return;
        }
        pwb->user_data = slot.release();
    }

    void on_remove_buffer(pw_buffer* pwb) {
        auto* slot = static_cast<Slot*>(pwb->user_data);
        pwb->user_data = nullptr;
        if (pwb == in_flight)
            in_flight = nullptr;  // the copy finishes into nothing
        if (!slot)
            return;
        if (slot->map) {
            // A copy may still hold it: it (and the slot) goes with its last lock.
            buffer_drop(&slot->memory_base);
            return;
        }
        if (slot->buffer)
            buffer_drop(slot->buffer);
        delete slot;
    }

    void arm(int64_t ns) {
        wl_event_source_timer_update(timer, std::max(1, int((ns + 999'999) / 1'000'000)));
    }

    // The next frame, if one may go and there is a buffer for it.
    void pump() {
        if (closed || !streaming || copying)
            return;
        int w, h;
        uint32_t drm;
        std::vector<uint64_t> mods;
        if (!constraints(&w, &h, &drm, &mods))
            return close_stream();
        if (w != width || h != height) {
            // The screen's mode or the window's size changed: say so.
            width = w;
            height = h;
            modifier.reset();
            update_formats(std::nullopt);
            return;
        }
        uint32_t num = format.max_framerate.num, den = format.max_framerate.denom;
        if (num == 0)
            num = max_rate, den = 1;
        if (const int64_t wait = screencast_wait_ns(last_frame, now_ns(), num, den))
            return arm(wait);
        pw_buffer* pwb = pw_stream_dequeue_buffer(stream);
        if (!pwb)
            return arm(4'000'000);  // the app holds them all: soon
        auto* slot = static_cast<Slot*>(pwb->user_data);
        if (!slot) {
            pw_stream_return_buffer(stream, pwb);
            return;
        }
        copying = true;
        in_flight = pwb;
        wl::Capture::Copy copy{
            .target = target,
            .buffer = slot->buffer,
            .wait_for_damage = !first,
            .done = [this, alive = std::weak_ptr<bool>(alive)](const wl::Capture::Result& r) {
                if (alive.expired())
                    return;
                copied(r);
            },
        };
        first = false;
        server().wl->capture->copy.emit(copy);
    }

    void copied(const wl::Capture::Result& r) {
        copying = false;
        pw_buffer* pwb = std::exchange(in_flight, nullptr);
        if (!r.ok) {
            if (pwb)
                pw_stream_return_buffer(stream, pwb);
            if (r.fail_reason == 2)
                return close_stream();  // the screen or window went
            if (r.fail_reason == 1)
                first = true;  // a new size: pump renegotiates
            return defer_pump();
        }
        if (!pwb)
            return defer_pump();
        if (modifier)
            server().renderer->finish();  // implicit sync is unreliable (NVIDIA): done before the app reads
        spa_buffer* sb = pwb->buffer;
        if (auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(spa_meta_header)))) {
            h->pts = now_ns();
            h->flags = 0;
            h->seq = seq++;
            h->dts_offset = 0;
        }
        if (spa_meta* m = spa_buffer_find_meta(sb, SPA_META_VideoDamage)) {
            auto* region = static_cast<spa_meta_region*>(spa_meta_first(m));
            auto put = [&](int x, int y, int w, int h) {
                if (!spa_meta_check(region, m))
                    return false;
                region->region = SPA_REGION(x, y, uint32_t(w), uint32_t(h));
                ++region;
                return true;
            };
            if (r.damage.empty() || int(r.damage.size()) >= kDamageRects)
                put(0, 0, width, height);
            else
                for (const Box& b : r.damage)
                    put(b.x, b.y, b.width, b.height);
            put(0, 0, 0, 0);  // the end
        }
        sb->datas[0].chunk->flags = SPA_CHUNK_FLAG_NONE;
        if (!modifier) {
            sb->datas[0].chunk->offset = 0;
            sb->datas[0].chunk->size = uint32_t(width * 4 * height);
            sb->datas[0].chunk->stride = width * 4;
        }
        pw_stream_queue_buffer(stream, pwb);
        last_frame = now_ns();
        defer_pump();
    }

    // Not from inside a copy's callback: on the next turn.
    void defer_pump() { wl_event_source_timer_update(timer, 1); }

    // `by_itself`: not asked to stop (its owner hears of it).
    void close_stream(bool by_itself = true) {
        if (closed)
            return;
        closed = true;
        if (ready)
            std::exchange(ready, {})(Ready{.error = "nothing to capture there"});
        else if (by_itself && ended)
            ended();
        ended = {};
        pw_stream_disconnect(stream);
        owner_->reap();
    }
};

ScreenCast::ScreenCast(Server& server) : server_(server) {
    pw_init(nullptr, nullptr);
    // A stream's screen or window went: it ends, whether or not anyone was
    // watching (no copy would have said so).
    stopped_ = server_.wl->capture->stopped.connect([this](const wl::Capture::Target& t) {
        for (auto& s : streams_)
            if (s->target.output == t.output && s->target.toplevel == t.toplevel)
                s->close_stream();
    });
}

ScreenCast::~ScreenCast() {
    stopped_.disconnect();
    streams_.clear();
    if (reap_)
        wl_event_source_remove(reap_);
    disconnect();
    pw_deinit();
}

bool ScreenCast::connect() {
    if (core_)
        return true;
    loop_ = pw_loop_new(nullptr);
    if (!loop_)
        return false;
    pw_loop_enter(loop_);
    source_ = wl_event_loop_add_fd(server_.loop, pw_loop_get_fd(loop_), WL_EVENT_READABLE,
                                   [](int, uint32_t, void* data) {
                                       pw_loop_iterate(static_cast<ScreenCast*>(data)->loop_, 0);
                                       return 0;
                                   }, this);
    context_ = pw_context_new(loop_, nullptr, 0);
    core_ = context_ ? pw_context_connect(context_, nullptr, 0) : nullptr;
    if (!core_) {
        alog(Log::Error, "screencast: can't reach PipeWire: %s", strerror(errno));
        disconnect();
        return false;
    }
    core_listener_ = std::make_unique<CoreListener>();
    core_listener_->events.version = PW_VERSION_CORE_EVENTS;
    core_listener_->events.error = [](void* data, uint32_t id, int, int res, const char* message) {
        auto* self = static_cast<ScreenCast*>(data);
        alog(Log::Error, "screencast: PipeWire: %s", message);
        // The daemon went (restarted): every stream ends, the next connects anew.
        if (id == PW_ID_CORE && res == -EPIPE) {
            for (auto& s : self->streams_) {
                if (!s->closed && !s->ready && s->ended)
                    s->ended();
                s->closed = true;
            }
            self->reap();
            self->drop_core_ = true;
        }
    };
    pw_core_add_listener(core_, &core_listener_->hook, &core_listener_->events, this);
    return true;
}

void ScreenCast::disconnect() {
    if (core_) {
        spa_hook_remove(&core_listener_->hook);
        pw_core_disconnect(core_);
    }
    core_ = nullptr;
    core_listener_.reset();
    if (context_)
        pw_context_destroy(context_);
    context_ = nullptr;
    if (source_)
        wl_event_source_remove(source_);
    source_ = nullptr;
    if (loop_) {
        pw_loop_leave(loop_);
        pw_loop_destroy(loop_);
    }
    loop_ = nullptr;
}

uint64_t ScreenCast::start(const wl::Capture::Target& target, uint64_t tag, std::function<void(const Ready&)> ready,
                           std::function<void()> ended) {
    if (!connect()) {
        ready(Ready{.error = "PipeWire isn't running"});
        return 0;
    }
    auto s = std::make_unique<Stream>();
    s->owner_ = this;
    s->id = next_id_++;
    s->tag = tag;
    s->target = target;
    s->ready = std::move(ready);
    s->ended = std::move(ended);
    if (!s->constraints(&s->width, &s->height, &s->drm_format, &s->modifiers)) {
        s->ready(Ready{.error = "nothing to capture there"});
        return 0;
    }
    if (target.output)
        if (auto* o = static_cast<Output*>(target.output->data); o && o->screen->refresh > 0)
            s->max_rate = uint32_t((o->screen->refresh + 500) / 1000);
    s->timer = wl_event_loop_add_timer(server_.loop, [](void* data) {
        static_cast<Stream*>(data)->pump();
        return 0;
    }, s.get());

    const std::string name = "atrium-screencast-" + std::to_string(s->id);
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_NAME, name.c_str(),
                                             PW_KEY_NODE_DESCRIPTION, "atrium screen cast", nullptr);
    s->stream = pw_stream_new(core_, name.c_str(), props);
    if (!s->stream) {
        s->ready(Ready{.error = "PipeWire refused the stream"});
        return 0;
    }
    s->events.version = PW_VERSION_STREAM_EVENTS;
    s->events.state_changed = [](void* d, pw_stream_state, pw_stream_state state, const char* error) {
        static_cast<Stream*>(d)->on_state(state, error);
    };
    s->events.param_changed = [](void* d, uint32_t id, const spa_pod* pod) {
        static_cast<Stream*>(d)->on_param(id, pod);
    };
    s->events.add_buffer = [](void* d, pw_buffer* b) { static_cast<Stream*>(d)->on_add_buffer(b); };
    s->events.remove_buffer = [](void* d, pw_buffer* b) { static_cast<Stream*>(d)->on_remove_buffer(b); };
    pw_stream_add_listener(s->stream, &s->listener, &s->events, s.get());

    uint8_t buf[4096];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    auto params = s->formats(&b, std::nullopt);
    const auto flags = pw_stream_flags(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS);
    if (pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params.data(), uint32_t(params.size())) <
        0) {
        s->ready(Ready{.error = "PipeWire refused the stream"});
        return 0;
    }
    const uint64_t id = s->id;
    streams_.push_back(std::move(s));
    return id;
}

bool ScreenCast::stop(uint64_t id) {
    for (auto& s : streams_)
        if (s->id == id) {
            s->close_stream(false);
            return true;
        }
    return false;
}

void ScreenCast::stop_owned(uint64_t tag) {
    for (auto& s : streams_)
        if (s->tag == tag)
            s->closed = true;
    reap();
}

// Closed streams go on the next turn, not inside their own callbacks.
void ScreenCast::reap() {
    if (reap_)
        return;
    reap_ = wl_event_loop_add_idle(server_.loop, [](void* data) {
        auto* self = static_cast<ScreenCast*>(data);
        self->reap_ = nullptr;
        std::erase_if(self->streams_, [](const std::unique_ptr<Stream>& s) {
            if (s->closed && s->ready)
                std::exchange(s->ready, {})(Ready{.error = "the stream ended"});
            return s->closed;
        });
        if (std::exchange(self->drop_core_, false) && self->streams_.empty())
            self->disconnect();
    }, this);
}

} // namespace atrium
