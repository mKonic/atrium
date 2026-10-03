// atrium-record: a screen, and what the speakers play, into an MP4: the
// shell's Record Screen. atrium makes the screen's PipeWire stream (IPC
// screencast.start, as for screen sharing; it ends with this connection),
// and this encodes it with the GPU's own H.264 encoder (NVENC, VA-API) or
// x264, the sound with AAC. SIGINT or SIGTERM finish the file.
//
//   atrium-record [-o SCREEN] [-f FPS] [-a] FILE

#include "record_core.hpp"

#include <nlohmann/json.hpp>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace atrium::record;

namespace {

int64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
}

std::string av_error(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

// --- atrium's IPC ----------------------------------------------------------------------

int connect_to(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return -1;
    std::strcpy(addr.sun_path, path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// $ATRIUM_SOCKET, else the one atrium socket in the runtime dir that answers.
int connect_atrium() {
    if (const char* s = std::getenv("ATRIUM_SOCKET"); s && *s)
        return connect_to(s);
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return -1;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(runtime, ec)) {
        const std::string name = e.path().filename();
        if (name.starts_with("atrium.") && name.ends_with(".sock"))
            if (const int fd = connect_to(e.path()); fd >= 0)
                return fd;
    }
    return -1;
}

struct Ipc {
    int fd = -1;
    std::string buf;

    bool send(const json& msg) {
        const std::string s = msg.dump() + "\n";
        for (size_t off = 0; off < s.size();) {
            const ssize_t n = write(fd, s.data() + off, s.size() - off);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            off += size_t(n);
        }
        return true;
    }

    // The reply to request `id`, past any events.
    std::optional<json> reply(int id) {
        for (;;) {
            if (auto nl = buf.find('\n'); nl != std::string::npos) {
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                json msg = json::parse(line, nullptr, false);
                if (msg.is_object() && msg.value("id", -1) == id)
                    return msg;
                continue;
            }
            char chunk[4096];
            const ssize_t n = read(fd, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return std::nullopt;
            buf.append(chunk, size_t(n));
        }
    }
};

// --- encoding ---------------------------------------------------------------------------

AVPixelFormat av_format(uint32_t spa) {
    switch (spa) {
    case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
    case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
    case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
    case SPA_VIDEO_FORMAT_xRGB_210LE: return AV_PIX_FMT_X2RGB10LE;
    case SPA_VIDEO_FORMAT_xBGR_210LE: return AV_PIX_FMT_X2BGR10LE;
    default: return AV_PIX_FMT_NONE;
    }
}

bool encoder_takes(const AVCodecContext* ctx, const AVCodec* codec, AVPixelFormat f) {
    const void* list = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) < 0 || !list)
        return false;
    const auto* formats = static_cast<const AVPixelFormat*>(list);
    for (int i = 0; i < n; ++i)
        if (formats[i] == f)
            return true;
    return false;
}

struct Recording {
    Options opt;
    AVFormatContext* mux = nullptr;
    // Video.
    AVCodecContext* video = nullptr;
    AVStream* video_stream = nullptr;
    AVBufferRef* hw_device = nullptr;
    AVBufferRef* hw_frames = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* last = nullptr;  // the last frame, as encoded (before any upload)
    AVPixelFormat in_format = AV_PIX_FMT_NONE;
    int width = 0, height = 0;  // what is encoded (even)
    Timeline clock;
    int64_t last_kept_us = -1;
    int64_t frames = 0;
    // Sound.
    AVCodecContext* audio = nullptr;
    AVStream* audio_stream = nullptr;
    AVAudioFifo* fifo = nullptr;
    int64_t next_audio_pts = -1;  // in samples
    bool failed = false;

    ~Recording() {
        av_frame_free(&last);
        sws_freeContext(sws);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
        av_buffer_unref(&hw_frames);
        av_buffer_unref(&hw_device);
        if (fifo)
            av_audio_fifo_free(fifo);
        if (mux) {
            if (mux->pb)
                avio_closep(&mux->pb);
            avformat_free_context(mux);
        }
    }

    bool write(AVCodecContext* enc, AVStream* st, AVFrame* frame) {
        int err = avcodec_send_frame(enc, frame);
        if (err < 0 && err != AVERROR_EOF) {
            std::fprintf(stderr, "atrium-record: encoding: %s\n", av_error(err).c_str());
            return false;
        }
        AVPacket* pkt = av_packet_alloc();
        while ((err = avcodec_receive_packet(enc, pkt)) >= 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (av_interleaved_write_frame(mux, pkt) < 0) {
                av_packet_free(&pkt);
                return false;
            }
        }
        av_packet_free(&pkt);
        return err == AVERROR(EAGAIN) || err == AVERROR_EOF;
    }

    bool open_mux() {
        if (avformat_alloc_output_context2(&mux, nullptr, "mp4", opt.file.c_str()) < 0)
            return false;
        return true;
    }

    // The first encoder that opens, for frames `w`x`h` of `format`.
    bool open_video(int w, int h, AVPixelFormat format) {
        width = w & ~1;
        height = h & ~1;
        in_format = format;
        std::vector<std::string> names;
        void* it = nullptr;
        while (const AVCodec* c = av_codec_iterate(&it))
            if (av_codec_is_encoder(c) && c->id == AV_CODEC_ID_H264)
                names.push_back(c->name);
        for (const std::string& name : encoders_to_try(names)) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            ctx->width = width;
            ctx->height = height;
            ctx->time_base = {1, 1'000'000};
            ctx->framerate = {opt.fps, 1};
            ctx->gop_size = opt.fps * 2;
            ctx->max_b_frames = 0;
            if (mux->oformat->flags & AVFMT_GLOBALHEADER)
                ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVPixelFormat sw = AV_PIX_FMT_YUV420P;
            if (name == "h264_nvenc") {
                // NVENC takes RGB itself and converts on the GPU.
                sw = encoder_takes(ctx, codec, format) ? format : AV_PIX_FMT_YUV420P;
                ctx->pix_fmt = sw;
                av_opt_set(ctx->priv_data, "preset", "p4", 0);
                av_opt_set(ctx->priv_data, "rc", "vbr", 0);
                av_opt_set(ctx->priv_data, "cq", "23", 0);
            } else if (name == "h264_vaapi") {
                sw = AV_PIX_FMT_NV12;
                if (av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
                    avcodec_free_context(&ctx);
                    continue;
                }
                hw_frames = av_hwframe_ctx_alloc(hw_device);
                auto* fc = reinterpret_cast<AVHWFramesContext*>(hw_frames->data);
                fc->format = AV_PIX_FMT_VAAPI;
                fc->sw_format = AV_PIX_FMT_NV12;
                fc->width = width;
                fc->height = height;
                fc->initial_pool_size = 8;
                if (av_hwframe_ctx_init(hw_frames) < 0) {
                    av_buffer_unref(&hw_frames);
                    av_buffer_unref(&hw_device);
                    avcodec_free_context(&ctx);
                    continue;
                }
                ctx->pix_fmt = AV_PIX_FMT_VAAPI;
                ctx->hw_frames_ctx = av_buffer_ref(hw_frames);
                ctx->global_quality = 23;
                av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0);
            } else {
                ctx->pix_fmt = sw;
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
                av_opt_set(ctx->priv_data, "crf", "21", 0);
            }
            if (const int err = avcodec_open2(ctx, codec, nullptr); err < 0) {
                std::fprintf(stderr, "atrium-record: %s won't open: %s\n", name.c_str(), av_error(err).c_str());
                avcodec_free_context(&ctx);
                av_buffer_unref(&hw_frames);
                av_buffer_unref(&hw_device);
                continue;
            }
            video = ctx;
            if (sw != format && ctx->pix_fmt != format)
                sws = sws_getContext(width, height, format, width, height, sw, SWS_BILINEAR, nullptr, nullptr,
                                     nullptr);
            video_stream = avformat_new_stream(mux, nullptr);
            avcodec_parameters_from_context(video_stream->codecpar, ctx);
            video_stream->time_base = ctx->time_base;
            std::fprintf(stderr, "atrium-record: %dx%d with %s\n", width, height, name.c_str());
            return true;
        }
        return false;
    }

    bool open_audio(int rate) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec)
            return false;
        audio = avcodec_alloc_context3(codec);
        audio->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audio->sample_rate = rate;
        av_channel_layout_default(&audio->ch_layout, 2);
        audio->bit_rate = 160'000;
        audio->time_base = {1, rate};
        if (mux->oformat->flags & AVFMT_GLOBALHEADER)
            audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(audio, codec, nullptr) < 0) {
            avcodec_free_context(&audio);
            return false;
        }
        audio_stream = avformat_new_stream(mux, nullptr);
        avcodec_parameters_from_context(audio_stream->codecpar, audio);
        audio_stream->time_base = audio->time_base;
        fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, rate);
        return true;
    }

    bool start_file() {
        if (avio_open(&mux->pb, opt.file.c_str(), AVIO_FLAG_WRITE) < 0)
            return false;
        // Fragments: a recording cut short (power, a crash) still plays.
        AVDictionary* o = nullptr;
        av_dict_set(&o, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        const int err = avformat_write_header(mux, &o);
        av_dict_free(&o);
        return err >= 0;
    }

    void video_frame(const uint8_t* data, int stride, int64_t ns) {
        if (failed || !video)
            return;
        const int64_t us = clock.video_us(ns);
        if (!keep_frame(us, last_kept_us, opt.fps))
            return;
        AVFrame src{};
        src.format = in_format;
        src.width = width;
        src.height = height;
        src.data[0] = const_cast<uint8_t*>(data);
        src.linesize[0] = stride;
        // Kept (converted, or copied out of PipeWire's buffer): a screen
        // that stays still sends nothing, and this is shown again.
        AVFrame* sw = av_frame_alloc();
        sw->format = sws ? (hw_frames ? AV_PIX_FMT_NV12 : video->pix_fmt) : in_format;
        sw->width = width;
        sw->height = height;
        if (av_frame_get_buffer(sw, 0) < 0) {
            av_frame_free(&sw);
            failed = true;
            return;
        }
        if (sws)
            sws_scale(sws, src.data, src.linesize, 0, height, sw->data, sw->linesize);
        else
            av_frame_copy(sw, &src);
        av_frame_free(&last);
        last = sw;
        encode_last(us);
    }

    // The last frame again, at `us`.
    void encode_last(int64_t us) {
        if (failed || !last)
            return;
        last_kept_us = us;
        AVFrame* frame = last;
        AVFrame* hw = nullptr;
        if (hw_frames) {
            hw = av_frame_alloc();
            if (av_hwframe_get_buffer(hw_frames, hw, 0) < 0 || av_hwframe_transfer_data(hw, last, 0) < 0)
                failed = true;
            frame = hw;
        }
        if (!failed) {
            frame->pts = us;
            failed = !write(video, video_stream, frame);
            ++frames;
        }
        av_frame_free(&hw);
    }

    // A still screen: its last frame once a second, and at the end, so the
    // video lasts as long as the recording.
    void hold(int64_t ns, bool end) {
        if (!clock.started() || !last)
            return;
        const auto us = clock.audio_us(ns);  // (the same clock, without moving it)
        if (us && *us > last_kept_us && (end || *us - last_kept_us >= 1'000'000))
            encode_last(clock.video_us(ns));
    }

    // `n` samples a channel, planar, captured ending about now.
    void audio_samples(float* const* planes, int n, int64_t ns) {
        if (failed || !audio || n <= 0)
            return;
        if (next_audio_pts < 0) {
            const auto us = clock.audio_us(ns);
            if (!us)
                return;  // before the first frame
            const int64_t at = *us * audio->sample_rate / 1'000'000;
            int skip = 0;
            if (at < 0)
                skip = int(std::min<int64_t>(-at, n));
            next_audio_pts = std::max<int64_t>(at, 0);
            if (skip >= n)
                return;
            float* shifted[2] = {planes[0] + skip, planes[1] + skip};
            av_audio_fifo_write(fifo, reinterpret_cast<void**>(shifted), n - skip);
        } else {
            av_audio_fifo_write(fifo, reinterpret_cast<void* const*>(const_cast<float**>(planes)), n);
        }
        drain_audio(false);
    }

    void drain_audio(bool all) {
        const int size = audio->frame_size > 0 ? audio->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= size || (all && av_audio_fifo_size(fifo) > 0)) {
            AVFrame* f = av_frame_alloc();
            f->nb_samples = std::min(size, av_audio_fifo_size(fifo));
            f->format = AV_SAMPLE_FMT_FLTP;
            f->sample_rate = audio->sample_rate;
            av_channel_layout_copy(&f->ch_layout, &audio->ch_layout);
            av_frame_get_buffer(f, 0);
            av_audio_fifo_read(fifo, reinterpret_cast<void**>(f->data), f->nb_samples);
            f->pts = next_audio_pts;
            next_audio_pts += f->nb_samples;
            if (!write(audio, audio_stream, f))
                failed = true;
            av_frame_free(&f);
            if (failed)
                return;
        }
    }

    bool finish() {
        if (audio) {
            drain_audio(true);
            write(audio, audio_stream, nullptr);
        }
        if (video)
            write(video, video_stream, nullptr);
        return av_write_trailer(mux) >= 0;
    }
};

// --- PipeWire -----------------------------------------------------------------------------

struct Capture {
    Recording* rec = nullptr;
    pw_thread_loop* loop = nullptr;
    pw_stream* video = nullptr;
    pw_stream* audio = nullptr;
    spa_hook video_listener{}, audio_listener{};
    spa_video_info_raw video_format{};
    bool video_ready = false;
    bool ended = false;
    std::string error;
    int audio_rate = 48000;
    spa_source* hold_timer = nullptr;
};

void on_hold(void* data, uint64_t) {
    auto* c = static_cast<Capture*>(data);
    c->rec->hold(now_ns(), false);
}

void video_param_changed(void* data, uint32_t id, const spa_pod* param) {
    auto* c = static_cast<Capture*>(data);
    if (id != SPA_PARAM_Format || !param)
        return;
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;
    c->video_format = info;
    // Memory: libav reads it on the CPU (NVENC and VA-API upload it).
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(4, 2, 8), SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))));
    pw_stream_update_params(c->video, params, 2);
}

void video_process(void* data) {
    auto* c = static_cast<Capture*>(data);
    pw_buffer* newest = nullptr;
    // Only the latest: older ones are already late.
    while (pw_buffer* b = pw_stream_dequeue_buffer(c->video)) {
        if (newest)
            pw_stream_queue_buffer(c->video, newest);
        newest = b;
    }
    if (!newest)
        return;
    spa_buffer* sb = newest->buffer;
    spa_data& d = sb->datas[0];
    if (d.data && d.chunk && d.chunk->size > 0 && !(d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        Recording& r = *c->rec;
        if (!r.video && !c->video_ready) {
            c->video_ready = true;
            const AVPixelFormat f = av_format(c->video_format.format);
            if (f == AV_PIX_FMT_NONE || !r.open_video(int(c->video_format.size.width),
                                                      int(c->video_format.size.height), f)) {
                c->error = "no H.264 encoder would open";
                c->ended = true;
            } else if (!r.start_file()) {
                c->error = "can't write " + r.opt.file;
                c->ended = true;
            }
            pw_thread_loop_signal(c->loop, false);
        }
        if (r.video && !c->ended) {
            int64_t ns = now_ns();
            if (auto* h = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(spa_meta_header))))
                if (h->pts > 0)
                    ns = h->pts;
            const auto* pixels = static_cast<const uint8_t*>(d.data) + d.chunk->offset;
            r.video_frame(pixels, d.chunk->stride, ns);
            if (r.failed) {
                c->error = "encoding failed";
                c->ended = true;
                pw_thread_loop_signal(c->loop, false);
            }
        }
    }
    pw_stream_queue_buffer(c->video, newest);
}

void video_state_changed(void* data, pw_stream_state, pw_stream_state state, const char* error) {
    auto* c = static_cast<Capture*>(data);
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        if (error)
            c->error = error;
        c->ended = true;
        pw_thread_loop_signal(c->loop, false);
    }
}

const pw_stream_events kVideoEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = video_state_changed,
    .param_changed = video_param_changed,
    .process = video_process,
};

void audio_param_changed(void* data, uint32_t id, const spa_pod* param) {
    auto* c = static_cast<Capture*>(data);
    if (id != SPA_PARAM_Format || !param)
        return;
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0)
        c->audio_rate = int(info.rate);
}

void audio_process(void* data) {
    auto* c = static_cast<Capture*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(c->audio);
    if (!b)
        return;
    spa_buffer* sb = b->buffer;
    if (sb->n_datas >= 2 && sb->datas[0].data && sb->datas[1].data && sb->datas[0].chunk) {
        const int n = int(sb->datas[0].chunk->size / sizeof(float));
        float* planes[2] = {
            reinterpret_cast<float*>(static_cast<uint8_t*>(sb->datas[0].data) + sb->datas[0].chunk->offset),
            reinterpret_cast<float*>(static_cast<uint8_t*>(sb->datas[1].data) + sb->datas[1].chunk->offset)};
        // Captured over the last n samples.
        const int64_t ns = now_ns() - int64_t(n) * 1'000'000'000 / c->audio_rate;
        c->rec->audio_samples(planes, n, ns);
    }
    pw_stream_queue_buffer(c->audio, b);
}

const pw_stream_events kAudioEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = audio_param_changed,
    .process = audio_process,
};

volatile sig_atomic_t stop_requested = 0;

void on_signal(int) {
    stop_requested = 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string why;
    const auto opt = parse_args(std::vector<std::string>(argv + 1, argv + argc), &why);
    if (!opt) {
        std::fprintf(stderr, "atrium-record: %s\nusage: atrium-record [-o SCREEN] [-f FPS] [-a] FILE\n",
                     why.c_str());
        return 2;
    }

    Ipc ipc;
    ipc.fd = connect_atrium();
    if (ipc.fd < 0) {
        std::fprintf(stderr, "atrium-record: atrium isn't running\n");
        return 1;
    }
    json req{{"cmd", "screencast.start"}, {"id", 1}, {"cursor", true}};
    if (!opt->output.empty())
        req["output"] = opt->output;
    const auto reply = ipc.send(req) ? ipc.reply(1) : std::nullopt;
    if (!reply || !reply->value("ok", false)) {
        std::fprintf(stderr, "atrium-record: no stream of the screen: %s\n",
                     reply ? reply->value("error", std::string("?")).c_str() : "atrium went");
        return 1;
    }
    const uint32_t node = (*reply)["result"].value("node", 0u);

    Recording rec;
    rec.opt = *opt;
    if (!rec.open_mux()) {
        std::fprintf(stderr, "atrium-record: can't make %s\n", opt->file.c_str());
        return 1;
    }

    pw_init(&argc, &argv);
    Capture cap;
    cap.rec = &rec;
    cap.loop = pw_thread_loop_new("atrium-record", nullptr);
    pw_context* context = pw_context_new(pw_thread_loop_get_loop(cap.loop), nullptr, 0);
    pw_core* core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
    if (!core) {
        std::fprintf(stderr, "atrium-record: PipeWire isn't running\n");
        return 1;
    }

    pw_thread_loop_lock(cap.loop);
    cap.video = pw_stream_new(core, "atrium-record",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    pw_stream_add_listener(cap.video, &cap.video_listener, &kVideoEvents, &cap);
    {
        uint8_t buf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        spa_rectangle def = SPA_RECTANGLE(1920, 1080), min = SPA_RECTANGLE(1, 1), max = SPA_RECTANGLE(16384, 16384);
        spa_fraction zero = SPA_FRACTION(0, 1), most = SPA_FRACTION(uint32_t(opt->fps), 1),
                     least = SPA_FRACTION(1, 1);
        const spa_pod* params[1] = {static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                   SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_xRGB_210LE,
                                   SPA_VIDEO_FORMAT_xBGR_210LE),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max), SPA_FORMAT_VIDEO_framerate,
            SPA_POD_Fraction(&zero), SPA_FORMAT_VIDEO_maxFramerate,
            SPA_POD_CHOICE_RANGE_Fraction(&most, &least, &most)))};
        pw_stream_connect(cap.video, PW_DIRECTION_INPUT, node,
                          pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    }
    if (opt->audio) {
        // What the speakers play: the default output's monitor.
        cap.audio = pw_stream_new(core, "atrium-record sound",
                                  pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                    PW_KEY_STREAM_CAPTURE_SINK, "true", nullptr));
        pw_stream_add_listener(cap.audio, &cap.audio_listener, &kAudioEvents, &cap);
        uint8_t buf[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        spa_audio_info_raw want{};
        want.format = SPA_AUDIO_FORMAT_F32P;
        want.rate = 48000;
        want.channels = 2;
        want.position[0] = SPA_AUDIO_CHANNEL_FL;
        want.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &want)};
        if (!rec.open_audio(48000)) {
            std::fprintf(stderr, "atrium-record: no AAC encoder; recording without sound\n");
            rec.opt.audio = false;
        } else {
            pw_stream_connect(cap.audio, PW_DIRECTION_INPUT, PW_ID_ANY,
                              // (On the loop's thread, as video is: one muxer.)
                              pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                              params, 1);
        }
    }
    {
        pw_loop* l = pw_thread_loop_get_loop(cap.loop);
        cap.hold_timer = pw_loop_add_timer(l, on_hold, &cap);
        timespec every{0, 250'000'000};
        pw_loop_update_timer(l, cap.hold_timer, &every, &every, false);
    }
    pw_thread_loop_unlock(cap.loop);

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    pw_thread_loop_start(cap.loop);

    // Until asked to stop, the stream ends (the screen went) or atrium does.
    pollfd p{ipc.fd, POLLIN, 0};
    while (!stop_requested) {
        pw_thread_loop_lock(cap.loop);
        const bool ended = cap.ended;
        pw_thread_loop_unlock(cap.loop);
        if (ended)
            break;
        if (poll(&p, 1, 200) > 0) {
            char chunk[4096];
            const ssize_t n = read(ipc.fd, chunk, sizeof chunk);
            if (n <= 0)
                break;
            ipc.buf.append(chunk, size_t(n));
            if (ipc.buf.find("screencast.ended") != std::string::npos)
                break;
            ipc.buf.clear();
        }
    }

    pw_thread_loop_lock(cap.loop);
    pw_loop_destroy_source(pw_thread_loop_get_loop(cap.loop), cap.hold_timer);
    rec.hold(now_ns(), true);
    if (cap.audio)
        pw_stream_disconnect(cap.audio);
    pw_stream_disconnect(cap.video);
    const bool wrote = rec.video && rec.mux->pb && rec.frames > 0;
    const bool ok = wrote && rec.finish();
    const std::string error = cap.error;
    pw_thread_loop_unlock(cap.loop);
    pw_thread_loop_stop(cap.loop);
    if (cap.audio)
        pw_stream_destroy(cap.audio);
    pw_stream_destroy(cap.video);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_thread_loop_destroy(cap.loop);
    close(ipc.fd);

    if (!ok) {
        std::fprintf(stderr, "atrium-record: nothing recorded%s%s\n", error.empty() ? "" : ": ", error.c_str());
        std::error_code ec;
        if (!wrote)
            std::filesystem::remove(opt->file, ec);
        return 1;
    }
    std::fprintf(stderr, "atrium-record: %lld frames to %s\n", static_cast<long long>(rec.frames),
                 opt->file.c_str());
    return 0;
}
