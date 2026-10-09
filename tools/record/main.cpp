// atrium-record: atrium's own gpu-screen-recorder. Records a screen (through
// ext-image-copy-capture) and, if asked, what the speakers play (PipeWire),
// into an MP4, taking gpu-screen-recorder's options:
//
//   atrium-record -w OUTPUT -o FILE [-f FPS] [-a default_output]
//                 [-k h264|hevc|av1] [-q medium|high|very_high|ultra] [-cursor yes|no]
//
// SIGINT (or SIGTERM) finishes the file. The picture goes to the GPU's
// encoder (NVENC, or VA-API), else to libx264, at a constant quantizer as
// gpu-screen-recorder's default; frames come as the screen changes (a
// variable frame rate) at most FPS a second.

#include "record_core.hpp"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace atrium::record;

namespace {

std::atomic<bool> g_stop{false};

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

std::string av_error(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

// --- capture ---------------------------------------------------------------------------

struct Shm {
    wl_buffer* wl = nullptr;
    uint8_t* data = nullptr;
    size_t size = 0;
    int stride = 0;
};

struct Capture {
    wl_shm* shm = nullptr;
    ext_image_copy_capture_manager_v1* copy = nullptr;
    ext_output_image_capture_source_manager_v1* sources = nullptr;
    struct Out {
        wl_output* wl;
        std::string name;
    };
    std::vector<std::unique_ptr<Out>> outputs;

    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    int width = 0, height = 0;
    uint32_t format = 0;
    bool have_format = false, ready = false, stopped = false;
    Shm buffer;
    // A frame just taken, waiting to be encoded.
    bool fresh = false;
    int64_t frame_ns = 0;
};

// The screen's formats as FFmpeg names them: the 32-bit ones, and the
// 24-bit ones some GPUs read back in (NVIDIA's BGR888).
AVPixelFormat pixel_format(uint32_t f) {
    switch (f) {
    case WL_SHM_FORMAT_XRGB8888:
    case WL_SHM_FORMAT_ARGB8888: return AV_PIX_FMT_BGR0;
    case WL_SHM_FORMAT_XBGR8888:
    case WL_SHM_FORMAT_ABGR8888: return AV_PIX_FMT_RGB0;
    case WL_SHM_FORMAT_BGR888: return AV_PIX_FMT_RGB24;
    case WL_SHM_FORMAT_RGB888: return AV_PIX_FMT_BGR24;
    default: return AV_PIX_FMT_NONE;
    }
}

int bytes_per_pixel(uint32_t f) {
    return f == WL_SHM_FORMAT_BGR888 || f == WL_SHM_FORMAT_RGB888 ? 3 : 4;
}

uint32_t preferred(uint32_t a, uint32_t b) {
    auto rank = [](uint32_t f) {
        return f == WL_SHM_FORMAT_XRGB8888 ? 0 : f == WL_SHM_FORMAT_XBGR8888 ? 1 : f == WL_SHM_FORMAT_ARGB8888 ? 2
             : f == WL_SHM_FORMAT_ABGR8888 ? 3 : 4;
    };
    return rank(a) <= rank(b) ? a : b;
}

const ext_image_copy_capture_frame_v1_listener kFrame = {
    .transform = [](void*, ext_image_copy_capture_frame_v1*, uint32_t) {},
    .damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t hi, uint32_t lo, uint32_t ns) {
        static_cast<Capture*>(data)->frame_ns = int64_t((uint64_t(hi) << 32) | lo) * 1000000000 + ns;
    },
    .ready = [](void* data, ext_image_copy_capture_frame_v1* f) {
        auto* c = static_cast<Capture*>(data);
        ext_image_copy_capture_frame_v1_destroy(f);
        c->frame = nullptr;
        c->fresh = true;
    },
    .failed = [](void* data, ext_image_copy_capture_frame_v1* f, uint32_t reason) {
        auto* c = static_cast<Capture*>(data);
        ext_image_copy_capture_frame_v1_destroy(f);
        c->frame = nullptr;
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)
            c->stopped = true;
    },
};

const ext_image_copy_capture_session_v1_listener kSession = {
    .buffer_size = [](void* data, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto* c = static_cast<Capture*>(data);
        if (c->ready && (int(w) != c->width || int(h) != c->height)) {
            std::fprintf(stderr, "atrium-record: the screen changed size; stopping\n");
            g_stop = true;
        }
        c->width = int(w);
        c->height = int(h);
    },
    .shm_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t f) {
        auto* c = static_cast<Capture*>(data);
        if (pixel_format(f) == AV_PIX_FMT_NONE)
            return;
        c->format = c->have_format ? preferred(c->format, f) : f;
        c->have_format = true;
    },
    .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
    .done = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<Capture*>(data)->ready = true; },
    .stopped = [](void* data, ext_image_copy_capture_session_v1*) { static_cast<Capture*>(data)->stopped = true; },
};

const wl_output_listener kOutput = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
    .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
    .done = [](void*, wl_output*) {},
    .scale = [](void*, wl_output*, int32_t) {},
    .name = [](void* data, wl_output*, const char* n) { static_cast<Capture::Out*>(data)->name = n; },
    .description = [](void*, wl_output*, const char*) {},
};

const wl_registry_listener kRegistry = {
    .global = [](void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
        auto* c = static_cast<Capture*>(data);
        auto is = [iface](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
        if (is(wl_shm_interface)) {
            c->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        } else if (is(wl_output_interface) && version >= 4) {
            auto o = std::make_unique<Capture::Out>();
            o->wl = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 4));
            wl_output_add_listener(o->wl, &kOutput, o.get());
            c->outputs.push_back(std::move(o));
        } else if (is(ext_image_copy_capture_manager_v1_interface)) {
            c->copy = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1));
        } else if (is(ext_output_image_capture_source_manager_v1_interface)) {
            c->sources = static_cast<ext_output_image_capture_source_manager_v1*>(
                wl_registry_bind(reg, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        }
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

bool make_shm(Capture& c) {
    c.buffer.stride = (c.width * bytes_per_pixel(c.format) + 3) & ~3;
    c.buffer.size = size_t(c.buffer.stride) * size_t(c.height);
    const int fd = memfd_create("atrium-record", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(c.buffer.size)) < 0)
        return false;
    void* p = mmap(nullptr, c.buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return false;
    }
    c.buffer.data = static_cast<uint8_t*>(p);
    wl_shm_pool* pool = wl_shm_create_pool(c.shm, fd, int32_t(c.buffer.size));
    c.buffer.wl = wl_shm_pool_create_buffer(pool, 0, c.width, c.height, c.buffer.stride, c.format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return true;
}

void request_frame(Capture& c) {
    c.frame = ext_image_copy_capture_session_v1_create_frame(c.session);
    ext_image_copy_capture_frame_v1_add_listener(c.frame, &kFrame, &c);
    ext_image_copy_capture_frame_v1_attach_buffer(c.frame, c.buffer.wl);
    ext_image_copy_capture_frame_v1_damage_buffer(c.frame, 0, 0, INT32_MAX, INT32_MAX);
    ext_image_copy_capture_frame_v1_capture(c.frame);
}

// --- sound -----------------------------------------------------------------------------

constexpr int kRate = 48000, kChannels = 2;

struct Sound {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    std::mutex lock;
    std::vector<float> samples;  // interleaved, waiting to be encoded
    int64_t first_ns = -1;       // when the first sample came
};

void sound_process(void* data) {
    auto* s = static_cast<Sound*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(s->stream);
    if (!b)
        return;
    spa_buffer* buf = b->buffer;
    if (buf->datas[0].data && buf->datas[0].chunk->size > 0) {
        const auto* p = static_cast<const float*>(buf->datas[0].data) + buf->datas[0].chunk->offset / sizeof(float);
        const size_t n = buf->datas[0].chunk->size / sizeof(float);
        std::lock_guard g(s->lock);
        if (s->first_ns < 0)
            s->first_ns = now_ns();
        s->samples.insert(s->samples.end(), p, p + n);
    }
    pw_stream_queue_buffer(s->stream, b);
}

bool start_sound(Sound& s) {
    s.loop = pw_thread_loop_new("atrium-record", nullptr);
    if (!s.loop)
        return false;
    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = sound_process;
        return e;
    }();
    // What the speakers play: the default sink's monitor.
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_STREAM_CAPTURE_SINK, "true",
                                             PW_KEY_NODE_NAME, "atrium-record", nullptr);
    s.stream = pw_stream_new_simple(pw_thread_loop_get_loop(s.loop), "Screen recording", props, &events, &s);
    if (!s.stream)
        return false;
    uint8_t storage[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof storage);
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    if (pw_stream_connect(s.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                          params, 1) < 0)
        return false;
    return pw_thread_loop_start(s.loop) == 0;
}

// --- encoding --------------------------------------------------------------------------

struct Encoder {
    AVFormatContext* mux = nullptr;
    AVCodecContext* video = nullptr;
    AVStream* video_stream = nullptr;
    AVBufferRef* hw_device = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;     // what the encoder takes (on the GPU for VA-API)
    AVFrame* sw_frame = nullptr;  // VA-API's: converted on the CPU first
    std::string name;

    AVCodecContext* audio = nullptr;
    AVStream* audio_stream = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* audio_frame = nullptr;
    int64_t audio_pts = 0;  // in samples
    bool audio_started = false;

    AVPacket* packet = nullptr;
};

bool nvidia() {
    return access("/proc/driver/nvidia/version", R_OK) == 0;
}

bool open_video(Encoder& e, const Options& o, int width, int height, AVPixelFormat input) {
    for (const std::string& name : encoders(o.codec, nvidia())) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec)
            continue;
        AVCodecContext* c = avcodec_alloc_context3(codec);
        c->width = width;
        c->height = height;
        c->time_base = {1, AV_TIME_BASE};  // a variable frame rate, in microseconds
        c->framerate = {o.fps, 1};
        c->gop_size = o.fps * 2;
        c->max_b_frames = 0;
        c->color_range = AVCOL_RANGE_MPEG;
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        c->colorspace = AVCOL_SPC_BT709;
        if (e.mux->oformat->flags & AVFMT_GLOBALHEADER)
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int qp = codec_qp(name == "libx264" ? Codec::H264 : o.codec, o.quality);
        AVDictionary* opts = nullptr;
        av_dict_set_int(&opts, "qp", qp, 0);
        AVBufferRef* device = nullptr;
        if (name.ends_with("_nvenc")) {
            // NVENC takes 32-bit pixels as they are and converts them
            // itself; 24-bit ones become NV12 first.
            c->pix_fmt = input == AV_PIX_FMT_BGR0 || input == AV_PIX_FMT_RGB0 ? input : AV_PIX_FMT_NV12;
            av_dict_set(&opts, "rc", "constqp", 0);
            av_dict_set(&opts, "tune", "ll", 0);
            av_dict_set_int(&opts, "forced-idr", 1, 0);
        } else if (name.ends_with("_vaapi")) {
            if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
                avcodec_free_context(&c);
                continue;
            }
            AVBufferRef* frames = av_hwframe_ctx_alloc(device);
            auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
            fc->format = AV_PIX_FMT_VAAPI;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = width;
            fc->height = height;
            fc->initial_pool_size = 4;
            if (av_hwframe_ctx_init(frames) < 0) {
                av_buffer_unref(&frames);
                av_buffer_unref(&device);
                avcodec_free_context(&c);
                continue;
            }
            c->pix_fmt = AV_PIX_FMT_VAAPI;
            c->hw_frames_ctx = frames;
            av_dict_set(&opts, "rc_mode", "CQP", 0);
            av_dict_set_int(&opts, "async_depth", 3, 0);
            c->global_quality = qp;
        } else {
            c->pix_fmt = AV_PIX_FMT_YUV420P;
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "tune", "film", 0);
        }
        if (codec->id == AV_CODEC_ID_H264)
            av_dict_set(&opts, "coder", "cabac", 0);
        const int err = avcodec_open2(c, codec, &opts);
        av_dict_free(&opts);
        if (err < 0) {
            std::fprintf(stderr, "atrium-record: %s didn't open (%s); trying the next\n", name.c_str(), av_error(err).c_str());
            avcodec_free_context(&c);
            av_buffer_unref(&device);
            continue;
        }
        e.video = c;
        e.hw_device = device;
        e.name = name;
        e.frame = av_frame_alloc();
        if (c->pix_fmt == AV_PIX_FMT_VAAPI) {
            e.sw_frame = av_frame_alloc();
            e.sw_frame->format = AV_PIX_FMT_NV12;
            e.sw_frame->width = width;
            e.sw_frame->height = height;
            av_frame_get_buffer(e.sw_frame, 0);
            e.sws = sws_getContext(width, height, input, width, height, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                   nullptr, nullptr, nullptr);
        } else {
            e.frame->format = c->pix_fmt;
            e.frame->width = width;
            e.frame->height = height;
            av_frame_get_buffer(e.frame, 0);
            if (c->pix_fmt != input)
                e.sws = sws_getContext(width, height, input, width, height, c->pix_fmt, SWS_BILINEAR,
                                       nullptr, nullptr, nullptr);
        }
        e.video_stream = avformat_new_stream(e.mux, nullptr);
        e.video_stream->time_base = c->time_base;
        avcodec_parameters_from_context(e.video_stream->codecpar, c);
        std::fprintf(stderr, "atrium-record: encoding with %s\n", name.c_str());
        return true;
    }
    return false;
}

bool open_audio(Encoder& e) {
    // Opus, as gpu-screen-recorder's default; AAC where there's no libopus.
    for (const char* name : {"libopus", "aac"}) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec)
            continue;
        AVCodecContext* c = avcodec_alloc_context3(codec);
        c->sample_rate = kRate;
        av_channel_layout_default(&c->ch_layout, kChannels);
        const AVSampleFormat* fmts = nullptr;
        avcodec_get_supported_config(c, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, reinterpret_cast<const void**>(&fmts), nullptr);
        c->sample_fmt = fmts ? fmts[0] : AV_SAMPLE_FMT_FLTP;
        c->bit_rate = 128000;
        c->time_base = {1, kRate};
        if (e.mux->oformat->flags & AVFMT_GLOBALHEADER)
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(c, codec, nullptr) < 0) {
            avcodec_free_context(&c);
            continue;
        }
        e.audio = c;
        e.audio_stream = avformat_new_stream(e.mux, nullptr);
        e.audio_stream->time_base = c->time_base;
        avcodec_parameters_from_context(e.audio_stream->codecpar, c);
        AVChannelLayout in = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&e.swr, &c->ch_layout, c->sample_fmt, kRate, &in, AV_SAMPLE_FMT_FLT, kRate, 0, nullptr);
        swr_init(e.swr);
        e.audio_frame = av_frame_alloc();
        e.audio_frame->format = c->sample_fmt;
        e.audio_frame->nb_samples = c->frame_size > 0 ? c->frame_size : 1024;
        av_channel_layout_copy(&e.audio_frame->ch_layout, &c->ch_layout);
        e.audio_frame->sample_rate = kRate;
        av_frame_get_buffer(e.audio_frame, 0);
        return true;
    }
    return false;
}

void drain(Encoder& e, AVCodecContext* c, AVStream* st) {
    while (avcodec_receive_packet(c, e.packet) == 0) {
        av_packet_rescale_ts(e.packet, c->time_base, st->time_base);
        e.packet->stream_index = st->index;
        av_interleaved_write_frame(e.mux, e.packet);
    }
}

void encode_video(Encoder& e, const Capture& cap, int64_t pts) {
    // The encoder's size: the screen's, made even.
    const int w = e.video->width, h = e.video->height;
    const uint8_t* src[1] = {cap.buffer.data};
    const int stride[1] = {cap.buffer.stride};
    if (e.sw_frame) {
        av_frame_make_writable(e.sw_frame);
        sws_scale(e.sws, src, stride, 0, h, e.sw_frame->data, e.sw_frame->linesize);
        av_frame_unref(e.frame);
        av_hwframe_get_buffer(e.video->hw_frames_ctx, e.frame, 0);
        av_hwframe_transfer_data(e.frame, e.sw_frame, 0);
    } else {
        av_frame_make_writable(e.frame);
        if (e.sws)
            sws_scale(e.sws, src, stride, 0, h, e.frame->data, e.frame->linesize);
        else
            for (int y = 0; y < h; y++)
                std::memcpy(e.frame->data[0] + size_t(y) * size_t(e.frame->linesize[0]),
                            cap.buffer.data + size_t(y) * size_t(cap.buffer.stride), size_t(w) * 4);  // 32-bit only
    }
    e.frame->pts = pts;
    if (avcodec_send_frame(e.video, e.frame) == 0)
        drain(e, e.video, e.video_stream);
}

void encode_audio(Encoder& e, Sound& s, int64_t start_ns, bool flush) {
    std::vector<float> take;
    int64_t first = -1;
    {
        std::lock_guard g(s.lock);
        take.swap(s.samples);
        first = s.first_ns;
    }
    if (!e.audio_started) {
        if (first < 0)
            return;
        // The first sound sits where it came in the recording.
        e.audio_pts = pts_us(first, start_ns) * kRate / 1000000;
        e.audio_started = true;
    }
    if (!take.empty()) {
        const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(take.data())};
        swr_convert(e.swr, nullptr, 0, in, int(take.size() / kChannels));
    }
    const int n = e.audio_frame->nb_samples;
    while (swr_get_out_samples(e.swr, 0) >= n || (flush && swr_get_out_samples(e.swr, 0) > 0)) {
        av_frame_make_writable(e.audio_frame);
        const int got = swr_convert(e.swr, e.audio_frame->data, n, nullptr, 0);
        if (got <= 0)
            break;
        if (got < n)  // the last bit: silence after it
            av_samples_set_silence(e.audio_frame->data, got, n - got, kChannels, AVSampleFormat(e.audio_frame->format));
        e.audio_frame->pts = e.audio_pts;
        e.audio_pts += n;
        if (avcodec_send_frame(e.audio, e.audio_frame) == 0)
            drain(e, e.audio, e.audio_stream);
    }
}

} // namespace

int main(int argc, char** argv) {
    const Parsed parsed = parse_args(std::vector<std::string>(argv + 1, argv + argc));
    if (!parsed.options) {
        std::fprintf(stderr, "atrium-record: %s\n"
                     "usage: atrium-record -w OUTPUT -o FILE [-f FPS] [-a default_output] [-k h264|hevc|av1]\n"
                     "                     [-q medium|high|very_high|ultra] [-cursor yes|no]\n",
                     parsed.error.c_str());
        return 2;
    }
    const Options& o = *parsed.options;
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });

    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "atrium-record: no Wayland display\n");
        return 1;
    }
    Capture cap;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, &cap);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!cap.copy || !cap.sources || !cap.shm) {
        std::fprintf(stderr, "atrium-record: the compositor has no ext-image-copy-capture\n");
        return 1;
    }
    wl_output* output = nullptr;
    for (auto& out : cap.outputs)
        if (out->name == o.output)
            output = out->wl;
    if (!output) {
        std::fprintf(stderr, "atrium-record: no screen named '%s'\n", o.output.c_str());
        return 1;
    }
    ext_image_capture_source_v1* source = ext_output_image_capture_source_manager_v1_create_source(cap.sources, output);
    cap.session = ext_image_copy_capture_manager_v1_create_session(
        cap.copy, source, o.cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(cap.session, &kSession, &cap);
    ext_image_capture_source_v1_destroy(source);
    while (!cap.ready && wl_display_dispatch(display) != -1) {
    }
    if (!cap.have_format || cap.width <= 0 || !make_shm(cap)) {
        std::fprintf(stderr, "atrium-record: the screen can't be copied into shared memory\n");
        return 1;
    }

    Encoder e;
    if (avformat_alloc_output_context2(&e.mux, nullptr, nullptr, o.file.c_str()) < 0 || !e.mux) {
        std::fprintf(stderr, "atrium-record: can't write '%s'\n", o.file.c_str());
        return 1;
    }
    // Encoders want even sizes.
    if (!open_video(e, o, cap.width & ~1, cap.height & ~1, pixel_format(cap.format))) {
        std::fprintf(stderr, "atrium-record: no video encoder works here\n");
        return 1;
    }
    Sound sound;
    if (o.audio) {
        pw_init(&argc, &argv);
        if (!open_audio(e) || !start_sound(sound)) {
            std::fprintf(stderr, "atrium-record: no sound: recording the picture only\n");
            e.audio = nullptr;
        }
    }
    if (avio_open(&e.mux->pb, o.file.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(e.mux, nullptr) < 0) {
        std::fprintf(stderr, "atrium-record: can't write '%s'\n", o.file.c_str());
        return 1;
    }
    e.packet = av_packet_alloc();

    const int64_t start = now_ns();
    // At most FPS a second; a little early, so a frame that lands on the
    // screen's refresh isn't put off to the next.
    const int64_t interval = std::max<int64_t>(0, 1000000000 / o.fps - 2000000);
    int64_t last_request = 0, last_pts = -1;
    request_frame(cap);
    last_request = now_ns();
    const int fd = wl_display_get_fd(display);
    while (!g_stop && !cap.stopped) {
        wl_display_flush(display);
        pollfd p{fd, POLLIN, 0};
        // Wake for the next request when one is owed, else for the sound.
        int wait_ms = 5;
        if (!cap.frame)
            wait_ms = int(std::clamp<int64_t>((interval - (now_ns() - last_request)) / 1000000, 0, 5));
        if (poll(&p, 1, wait_ms) > 0 && wl_display_dispatch(display) < 0)
            break;
        wl_display_dispatch_pending(display);
        // A new picture, or the last one again after a second of nothing
        // changing (a still screen still has a length, and seeks).
        const bool stale = last_pts >= 0 && pts_us(now_ns(), start) - last_pts >= 1000000;
        if (cap.fresh || stale) {
            int64_t pts = pts_us(cap.fresh && cap.frame_ns ? cap.frame_ns : now_ns(), start);
            cap.fresh = false;
            if (pts <= last_pts)
                pts = last_pts + 1;
            last_pts = pts;
            encode_video(e, cap, pts);
        }
        // The next frame no sooner than the frame rate allows.
        if (!cap.frame && now_ns() - last_request >= interval) {
            request_frame(cap);
            last_request = now_ns();
        }
        if (e.audio)
            encode_audio(e, sound, start, false);
    }

    // Finish: what's left in the encoders, then the file's index.
    if (sound.loop)
        pw_thread_loop_stop(sound.loop);
    if (e.audio) {
        encode_audio(e, sound, start, true);
        avcodec_send_frame(e.audio, nullptr);
        drain(e, e.audio, e.audio_stream);
    }
    // The picture as it was at the end, so the video lasts until then.
    if (last_pts >= 0)
        encode_video(e, cap, std::max(last_pts + 1, pts_us(now_ns(), start)));
    avcodec_send_frame(e.video, nullptr);
    drain(e, e.video, e.video_stream);
    av_write_trailer(e.mux);
    avio_closep(&e.mux->pb);
    // Freed before exiting: NVENC's CUDA threads hang an exit that leaves them.
    avcodec_free_context(&e.video);
    avcodec_free_context(&e.audio);
    av_frame_free(&e.frame);
    av_frame_free(&e.sw_frame);
    av_frame_free(&e.audio_frame);
    av_packet_free(&e.packet);
    av_buffer_unref(&e.hw_device);
    sws_freeContext(e.sws);
    swr_free(&e.swr);
    avformat_free_context(e.mux);
    if (sound.stream)
        pw_stream_destroy(sound.stream);
    if (sound.loop)
        pw_thread_loop_destroy(sound.loop);
    wl_display_disconnect(display);
    std::fprintf(stderr, "atrium-record: saved %s\n", o.file.c_str());
    return 0;
}
