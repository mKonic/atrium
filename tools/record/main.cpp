// atrium-record: atrium's own gpu-screen-recorder (design/recorder.md). Takes
// its recording options, captures a screen as GPU buffers
// (ext-image-copy-capture into dmabufs), converts them in a shader into the
// encoder's planes on the same GPU (NVENC through CUDA, VA-API, Vulkan, or
// libx264 with -encoder cpu), and records sound from PipeWire, on a fixed
// clock that never waits on the compositor.
//
//   atrium-record -w DP-1 -f 60 -a default_output -o video.mp4
//
// SIGINT/SIGTERM save and stop, SIGUSR2 pauses and resumes. The saved path is
// printed on stdout; exit codes are gpu-screen-recorder's.

#include "audio.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "gpu.hpp"
#include "record_core.hpp"
#include "screen_color.hpp"
#include "version.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace atrium::record;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_toggle_pause{0};

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

[[noreturn]] void finish(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    // gsr: CUDA's atexit handlers can hang the exit on some NVIDIA drivers.
    _exit(code);
}

[[noreturn]] void die(int code, const std::string& message) {
    std::fprintf(stderr, "atrium-record: %s\n", message.c_str());
    finish(code);
}

// gsr's set_environment_variables.
void set_environment(bool low_power) {
    setenv("CUDA_DISABLE_PERF_BOOST", "1", 1);  // NVENC otherwise pins the GPU at P2
    setenv("__GL_MaxFramesAllowed", "1", 1);
    setenv("__GL_THREADED_OPTIMIZATIONS", "0", 1);  // else cuGraphicsGLRegisterImage fails on EGL
    unsetenv("LIBVA_DRIVER_NAME");
    unsetenv("__GL_SYNC_TO_VBLANK");
    unsetenv("vblank_mode");
    if (!low_power)
        setenv("AMD_DEBUG", "lowlatencyenc", 1);
}

// gsr's install_cuda_no_stable_perf_limit: an application profile so CUDA
// doesn't lock the memory clock down while recording.
void install_cuda_profile() {
    if (access("/proc/driver/nvidia/version", F_OK) != 0)
        return;
    const char* home = std::getenv("HOME");
    if (!home)
        return;
    const std::filesystem::path dir = std::filesystem::path(home) / ".nv" / "nvidia-application-profiles-rc.d";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / "10-atrium-record-cuda-no-stable-perf-limit";
    if (std::filesystem::exists(file, ec))
        return;
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
        std::fputs("{\n"
                   "    \"profiles\": [\n"
                   "        {\n"
                   "            \"name\": \"CudaNoStablePerfLimit\",\n"
                   "            \"settings\": [\"0x166c5e\", 0]\n"
                   "        }\n"
                   "    ],\n"
                   "    \"rules\": [\n"
                   "        { \"pattern\": \"atrium-record\", \"profile\": \"CudaNoStablePerfLimit\" }\n"
                   "    ]\n"
                   "}\n",
                   f);
        std::fclose(f);
    }
}

bool piped(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISCHR(st.st_mode));
}

// -sc: run with the saved file and its kind, detached (gsr double-forks).
void run_script(const std::string& script, const std::string& file) {
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            execl(script.c_str(), script.c_str(), file.c_str(), "regular", static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

// The output file and its streams, written as gsr's muxer.c/encoder.c do.
struct Output {
    AVFormatContext* fmt = nullptr;
    AVStream* video = nullptr;
    std::vector<AVStream*> audio;
    bool written = false, hybrid = false;
    std::string ts_file;  // -write-first-frame-ts
    AVPacket* packet = av_packet_alloc();

    void write(AVCodecContext* ctx, AVStream* st, bool is_audio, int64_t& audio_pts) {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            if (is_audio) {
                // gsr: audio counts its own time (a frame a packet) and what
                // falls before the start is dropped.
                packet->pts = packet->dts = audio_pts;
                audio_pts += ctx->frame_size;
                if (packet->pts < 0) {
                    av_packet_unref(packet);
                    continue;
                }
            }
            packet->stream_index = st->index;
            av_packet_rescale_ts(packet, ctx->time_base, st->time_base);
            if (!ts_file.empty() && !is_audio) {
                if (FILE* f = std::fopen(ts_file.c_str(), "w")) {
                    timespec mono, real;
                    clock_gettime(CLOCK_MONOTONIC, &mono);
                    clock_gettime(CLOCK_REALTIME, &real);
                    std::fprintf(f, "monotonic_microsec\trealtime_microsec\n%lld\t%lld\n",
                                 (long long)mono.tv_sec * 1000000 + mono.tv_nsec / 1000,
                                 (long long)real.tv_sec * 1000000 + real.tv_nsec / 1000);
                    std::fclose(f);
                }
                ts_file.clear();
            }
            // Not interleaved: gsr found av_interleaved_write_frame skipping
            // and duplicating frames.
            const int err = av_write_frame(fmt, packet);
            if (err >= 0)
                written = true;
            else
                std::fprintf(stderr, "atrium-record: failed to write a packet (%s)\n", av_error(err).c_str());
            av_packet_unref(packet);
        }
    }
};

struct AudioEncoder {
    AVCodecContext* ctx = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    int64_t pts = 0;         // the next frame's, in samples (starts before 0 by the codec's head start)
    int64_t packet_pts = 0;  // the next packet's
    std::vector<float> buffer;
};

AVCodecContext* open_audio(AudioCodec c, const Options& o, bool mixed, std::string& error) {
    const AVCodecID id = c == AudioCodec::Aac ? AV_CODEC_ID_AAC : c == AudioCodec::Mp3 ? AV_CODEC_ID_MP3 : AV_CODEC_ID_OPUS;
    const AVCodec* codec = avcodec_find_encoder(id);
    if (!codec) {
        error = std::string("FFmpeg has no ") + avcodec_get_name(id) + " encoder";
        return nullptr;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    // gsr's audio_codec.c: opus S16 when it can (float when devices are
    // mixed), aac planar float.
    const AVSampleFormat* fmts = nullptr;
    int n = 0;
    avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, reinterpret_cast<const void**>(&fmts), &n);
    std::vector<AVSampleFormat> want = c == AudioCodec::Opus && !mixed
                                           ? std::vector<AVSampleFormat>{AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP}
                                           : std::vector<AVSampleFormat>{AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P,
                                                                         AV_SAMPLE_FMT_S16};
    ctx->sample_fmt = fmts && n > 0 ? fmts[0] : AV_SAMPLE_FMT_FLTP;
    for (AVSampleFormat w : want)
        if (fmts && std::find(fmts, fmts + n, w) != fmts + n) {
            ctx->sample_fmt = w;
            break;
        }
    ctx->bit_rate = int64_t(o.audio_bitrate > 0 ? o.audio_bitrate : audio_bitrate(c)) * 1000;
    ctx->sample_rate = kSampleRate;
    if (c == AudioCodec::Aac)
        ctx->profile = AV_PROFILE_AAC_LOW;
    av_channel_layout_default(&ctx->ch_layout, kChannels);
    ctx->time_base = {1, kSampleRate};
    ctx->thread_count = 1;
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "strict", "experimental", 0);
    if (!o.ffmpeg_audio_opts.empty())
        av_dict_parse_string(&opts, o.ffmpeg_audio_opts.c_str(), "=", ";", 0);
    const int err = avcodec_open2(ctx, codec, &opts);
    av_dict_free(&opts);
    if (err < 0) {
        error = std::string("the ") + codec->name + " audio encoder wouldn't open (" + av_error(err) + ")";
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

// The screen to record: by name, "screen" (the first), or the one holding a
// region's centre.
Screen* find_screen(Capture& cap, const Options& o) {
    auto& screens = cap.screens();
    if (screens.empty())
        return nullptr;
    if (o.source == "screen")
        return screens.front().get();
    if (o.source == "region") {
        const int cx = o.region->x + o.region->width / 2, cy = o.region->y + o.region->height / 2;
        for (auto& s : screens)
            if (cx >= s->x && cy >= s->y && cx < s->x + s->logical_width && cy < s->y + s->logical_height)
                return s.get();
        return nullptr;
    }
    for (auto& s : screens)
        if (s->name == o.source)
            return s.get();
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    mallopt(M_MMAP_THRESHOLD, 65536);
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    std::signal(SIGUSR2, [](int) { g_toggle_pause++; });
    // Replay's signals (gsr: save replay, toggle replay recording): no
    // replay here yet, so they do nothing rather than kill the recording.
    std::signal(SIGUSR1, SIG_IGN);
    for (int i = 0; i <= 6; i++)
        std::signal(SIGRTMIN + i, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);

    if (geteuid() == 0)
        die(kFailed, "don't run atrium-record as the root user");

    const Parsed parsed = parse_args(std::vector<std::string>(argv + 1, argv + argc));
    if (parsed.help) {
        std::fputs(kUsage, stdout);
        finish(kOk);
    }
    if (parsed.version) {
        std::printf("%s\n", ATRIUM_VERSION);
        finish(kOk);
    }
    if (!parsed.options) {
        std::fprintf(stderr, "atrium-record: %s\n%s", parsed.error.c_str(), kUsage);
        finish(parsed.exit);
    }
    Options o = *parsed.options;
    set_environment(o.low_power);
    install_cuda_profile();

    if (!o.script.empty()) {
        struct stat st;
        if (stat(o.script.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            die(kFailed, "Script \"" + o.script + "\" either doesn't exist or it's not a file");
        if (!(st.st_mode & S_IXUSR))
            die(kFailed, "Script \"" + o.script + "\" is not executable");
    }
    if (o.file.empty())
        o.file = "/dev/stdout";
    const bool is_pipe = piped(o.file);
    if (!is_pipe) {
        const std::filesystem::path dir = std::filesystem::path(o.file).parent_path();
        std::error_code ec;
        if (!dir.empty())
            std::filesystem::create_directories(dir, ec);
        if (ec)
            die(kFailed, "failed to create directory for output file: " + o.file);
    } else if (o.write_first_frame_ts) {
        std::fprintf(stderr, "atrium-record: warning: -write-first-frame-ts is ignored when output is piped\n");
        o.write_first_frame_ts = false;
    }
    if (is_pipe && !o.script.empty())
        o.script.clear();
    // The extension gsr's rules go by: -c's, else the file's.
    const std::string ext = o.container == "matroska" ? "mkv" : !o.container.empty() ? o.container : extension(o.file);
    const bool low_latency = is_pipe;

    // --- video: the screen, the GPU, the encoder ---------------------------------------
    std::unique_ptr<Capture> cap;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<VideoEncoder> video;
    ScreenColorWatch colors;
    Screen* screen = nullptr;
    int crop[4] = {0, 0, 0, 0};
    Size video_size;
    const int64_t interval = 1000000000LL / o.fps;
    if (!o.source.empty()) {
        cap = std::make_unique<Capture>();
        std::string error;
        if (!cap->connect(error))
            die(kCaptureFailed, error);
        screen = find_screen(*cap, o);
        if (!screen) {
            std::string list;
            for (auto& s : cap->screens())
                list += "    " + s->name + " (" + std::to_string(s->width) + "x" + std::to_string(s->height) + ")\n";
            die(kMonitorMissing, (o.source == "region" ? "no monitor holds the region's centre" : "\"" + o.source + "\" isn't a monitor") +
                                     ", expected one of:\n" + list);
        }
        if (!colors.start(screen->name) && o.verbose)
            std::fprintf(stderr, "atrium-record: no atrium to ask about HDR and night light; taking the screen as SDR\n");
        const ScreenColor color = colors.get();
        if (!cap->start(*screen, o.cursor, color.hdr, interval, error))
            die(kCaptureFailed, error);
        gpu = std::make_unique<Gpu>();
        if (!gpu->open(cap->render_node(), o.gl_debug, error))
            die(kGlFailed, error);

        // The picture: the screen upright, or a region of it in its pixels.
        const bool sideways = screen->transform & 1;
        Size upright = sideways ? Size{cap->height(), cap->width()} : Size{cap->width(), cap->height()};
        Size picture = upright;
        if (o.region && o.region->width > 0 && o.region->height > 0 && screen->logical_width > 0) {
            const double sx = double(upright.width) / screen->logical_width, sy = double(upright.height) / screen->logical_height;
            crop[0] = int((o.region->x - screen->x) * sx);
            crop[1] = int((o.region->y - screen->y) * sy);
            crop[2] = std::max(1, int(o.region->width * sx));
            crop[3] = std::max(1, int(o.region->height * sy));
            picture = {crop[2], crop[3]};
        }
        video_size = fit(picture, o.size);
        ColorSetup setup{o.codec.ten_bit, o.codec.hdr, o.full_range};
        if (!gpu->prepare(setup, error))
            die(kGlFailed, error);
        int code = kFailed;
        video = VideoEncoder::open(*gpu, o, ext, video_size, low_latency, code, error);
        if (!video)
            die(code, error);
        if (video->ten_bit() != setup.ten_bit || video->hdr() != setup.hdr_video) {
            setup.ten_bit = video->ten_bit();
            setup.hdr_video = video->hdr();
            if (!gpu->prepare(setup, error))
                die(kGlFailed, error);
        }
        if (o.verbose)
            std::fprintf(stderr, "atrium-record: %s %dx%d from %s on %s (%s)%s\n", video->name().c_str(), video->context()->width,
                         video->context()->height, screen->name.c_str(), cap->render_node().c_str(), gpu->vendor().c_str(),
                         setup.hdr_video ? ", HDR" : "");
    }

    // --- sound --------------------------------------------------------------------------
    Clock clock;
    std::unique_ptr<AudioCapture> sound;
    std::vector<AudioEncoder> audio(o.audio.size());
    AudioCodec acodec = AudioCodec::Opus;
    bool mixed = false;
    for (const AudioTrack& t : o.audio)
        mixed |= t.sources.size() > 1;
    if (!o.audio.empty()) {
        std::string why;
        acodec = audio_codec_for(o.audio_codec, ext, mixed, &why);
        if (o.audio_codec_given && !why.empty())
            std::fprintf(stderr, "atrium-record: warning: %s\n", why.c_str());
        for (AudioEncoder& a : audio) {
            std::string error;
            a.ctx = open_audio(acodec, o, mixed, error);
            if (!a.ctx)
                die(kFailed, error);
            if (a.ctx->frame_size <= 0)
                a.ctx->frame_size = audio_frame_size(acodec);
        }
        // gsr's head start for the sound (its codec's delay), unless the
        // container can't take sound before 0.
        const bool offset_ok = (ext == "mp4" || ext == "mkv" || ext == "webm") && !low_latency;
        const int64_t shift = offset_ok ? int64_t(audio_delay(acodec, o.fps) * kSampleRate) : 0;
        for (AudioEncoder& a : audio) {
            a.pts = a.packet_pts = -shift;
            a.frame = av_frame_alloc();
            a.frame->format = a.ctx->sample_fmt;
            a.frame->nb_samples = a.ctx->frame_size;
            a.frame->sample_rate = kSampleRate;
            av_channel_layout_copy(&a.frame->ch_layout, &a.ctx->ch_layout);
            av_frame_get_buffer(a.frame, 0);
            const AVChannelLayout in = AV_CHANNEL_LAYOUT_STEREO;
            swr_alloc_set_opts2(&a.swr, &a.ctx->ch_layout, a.ctx->sample_fmt, kSampleRate, &in, AV_SAMPLE_FMT_FLT, kSampleRate, 0,
                                nullptr);
            swr_init(a.swr);
            a.buffer.resize(size_t(a.ctx->frame_size) * kChannels);
        }
        sound = std::make_unique<AudioCapture>();
        int code = kFailed;
        std::string error;
        if (!sound->start(o.audio, audio[0].ctx->frame_size, clock, code, error))
            die(code, error);
    }

    // --- the file -----------------------------------------------------------------------
    Output out;
    avformat_alloc_output_context2(&out.fmt, nullptr, o.container.empty() ? nullptr : o.container.c_str(), o.file.c_str());
    if (!out.fmt)
        die(kFailed, "failed to create output context for '" + o.file + "'");
    if (const AVOption* flags = av_opt_find(out.fmt->priv_data, "movflags", nullptr, 0, 0); flags && flags->unit) {
        if (is_pipe) {
            // Nothing to seek back into: fragments from the start.
            av_opt_set(out.fmt->priv_data, "movflags", "+frag_keyframe+empty_moov", 0);
        } else if (av_opt_find(out.fmt->priv_data, "hybrid_fragmented", flags->unit, 0, 0)) {
            // Crash-safe: fragmented while recording, a normal MP4 at the end.
            av_opt_set(out.fmt->priv_data, "use_editlist", "1", 0);
            av_opt_set(out.fmt->priv_data, "movflags", "+hybrid_fragmented", 0);
            out.hybrid = true;
        } else {
            std::fprintf(stderr, "atrium-record: warning: this FFmpeg has no hybrid_fragmented mp4s; a killed recording won't play\n");
        }
    }
    if (video) {
        AVCodecContext* vc = video->context();
        out.video = avformat_new_stream(out.fmt, nullptr);
        out.video->id = int(out.fmt->nb_streams) - 1;
        out.video->time_base = vc->time_base;
        out.video->avg_frame_rate = vc->framerate;
        avcodec_parameters_from_context(out.video->codecpar, vc);
        if (video->hdr()) {
            // atrium's copies are SDR: gsr's metadata for an SDR source,
            // everything at most the reference white.
            const double peak = 203.0;
            size_t size = 0;
            AVMasteringDisplayMetadata* m = av_mastering_display_metadata_alloc_size(&size);
            const double prim[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};  // BT.2020
            for (int i = 0; i < 3; i++) {
                m->display_primaries[i][0] = av_d2q(prim[i][0], 50000);
                m->display_primaries[i][1] = av_d2q(prim[i][1], 50000);
            }
            m->white_point[0] = av_d2q(0.3127, 50000);
            m->white_point[1] = av_d2q(0.3290, 50000);
            m->min_luminance = av_d2q(0.0, 10000);
            m->max_luminance = av_d2q(peak, 10000);
            m->has_primaries = m->has_luminance = 1;
            size_t light_size = 0;
            AVContentLightMetadata* l = av_content_light_metadata_alloc(&light_size);
            l->MaxCLL = unsigned(peak);
            l->MaxFALL = 0;
            av_packet_side_data_add(&out.video->codecpar->coded_side_data, &out.video->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_MASTERING_DISPLAY_METADATA, m, size, 0);
            av_packet_side_data_add(&out.video->codecpar->coded_side_data, &out.video->codecpar->nb_coded_side_data,
                                    AV_PKT_DATA_CONTENT_LIGHT_LEVEL, l, light_size, 0);
        }
    }
    for (size_t i = 0; i < audio.size(); i++) {
        AVStream* st = avformat_new_stream(out.fmt, nullptr);
        st->id = int(out.fmt->nb_streams) - 1;
        st->time_base = audio[i].ctx->time_base;
        avcodec_parameters_from_context(st->codecpar, audio[i].ctx);
        const std::string title = sound->title(i);
        if (!title.empty() && !o.exclude_metadata)
            av_dict_set(&st->metadata, "title", title.c_str(), 0);
        out.audio.push_back(st);
    }
    if (int err = avio_open(&out.fmt->pb, o.file.c_str(), AVIO_FLAG_WRITE); err < 0)
        die(kFailed, "could not open '" + o.file + "': " + av_error(err));
    {
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "strict", "experimental", 0);
        if (!o.ffmpeg_opts.empty())
            av_dict_parse_string(&opts, o.ffmpeg_opts.c_str(), "=", ";", 0);
        const int err = avformat_write_header(out.fmt, &opts);
        av_dict_free(&opts);
        if (err < 0)
            die(kFailed, "error occurred when writing header to output file: " + av_error(err));
    }
    if (o.write_first_frame_ts && video)
        out.ts_file = o.file + ".ts";

    // --- recording ----------------------------------------------------------------------
    // The clock starts with the first frame (a moment after the copy is
    // asked for), so the video starts at 0.
    if (cap) {
        const int64_t give_up = now_ns() + 2000000000LL;
        while (!cap->latest() && !cap->stopped() && now_ns() < give_up && !g_stop) {
            cap->pump(now_ns());
            cap->flush();
            pollfd p{cap->fd(), POLLIN, 0};
            if (poll(&p, 1, 50) > 0 && !cap->dispatch())
                break;
        }
        if (!cap->latest())
            die(kCaptureFailed, "no frame came from the compositor");
    }
    clock.start(now_ns());

    int64_t last_index = -1, last_pts = -1, cfr_pts = 0;
    uint64_t last_frame_count = 0;
    int64_t no_audio_pts = 0;  // Output::write's counter, unused for video
    auto encode_audio = [&](bool flush) {
        const int64_t rec = clock.at(now_ns());
        for (size_t i = 0; i < audio.size(); i++) {
            AudioEncoder& a = audio[i];
            int64_t pos = 0;
            // 300 ms: a device that has sent nothing by then is silent.
            while (sound->pop(i, a.ctx->frame_size, flush ? INT64_MAX / 4 : rec, 300000000, a.buffer.data(), pos)) {
                const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(a.buffer.data())};
                av_frame_make_writable(a.frame);
                swr_convert(a.swr, a.frame->data, a.ctx->frame_size, in, a.ctx->frame_size);
                a.frame->pts = a.pts;
                a.pts += a.ctx->frame_size;
                if (avcodec_send_frame(a.ctx, a.frame) == 0)
                    out.write(a.ctx, out.audio[i], true, a.packet_pts);
                if (flush && pos * 1000000000 / kSampleRate > clock.at(now_ns()))
                    break;
            }
        }
    };

    auto encode_video = [&](int64_t rec_ns) {
        const Dmabuf* frame = cap->latest();
        const Planes* planes = video->begin();
        if (!frame || !planes)
            return;
        AVCodecContext* vc = video->context();
        if (!gpu->draw(*frame, cap->frame_transform(), crop, colors.get(), *planes, video_size.width, video_size.height))
            return;
        if (gpu->nvidia())
            glFinish();  // gsr: without it the cursor flickers on NVIDIA
        AVFrame* f = video->end();
        if (!f)
            return;
        f->color_range = vc->color_range;
        f->color_primaries = vc->color_primaries;
        f->color_trc = vc->color_trc;
        f->colorspace = vc->colorspace;
        f->chroma_location = AVCHROMA_LOC_LEFT;
        const int64_t index = frame_index(rec_ns, o.fps);
        if (o.fm == FrameRateMode::Cfr) {
            // Every frame the clock passed, the missed ones as copies.
            const int64_t missed = last_index < 0 ? 1 : std::max<int64_t>(1, index - last_index);
            for (int64_t i = 0; i < missed; i++) {
                AVFrame* copy = av_frame_clone(f);
                copy->pts = cfr_pts++;
                if (avcodec_send_frame(vc, copy) == 0)
                    out.write(vc, out.video, false, no_audio_pts);
                av_frame_free(&copy);
            }
        } else {
            const int64_t pts = rec_ns / 1000;
            if (pts != last_pts) {
                f->pts = pts;
                last_pts = pts;
                if (avcodec_send_frame(vc, f) == 0)
                    out.write(vc, out.video, false, no_audio_pts);
            }
        }
        av_frame_free(&f);
        last_index = index;
        last_frame_count = cap->frames();
    };

    bool paused = false;
    int handled_pauses = 0;
    while (!g_stop) {
        if (int toggles = g_toggle_pause.load(); toggles != handled_pauses) {
            handled_pauses = toggles;
            paused = !paused;
            clock.set_paused(paused, now_ns());
            std::fprintf(stderr, "atrium-record: %s\n", paused ? "paused" : "resumed");
        }
        const int64_t now = now_ns();
        int64_t wake = now + 10000000;  // the sound, every 10 ms at least
        if (cap) {
            cap->pump(now);
            if (!cap->flush())
                break;
            const int64_t rec = clock.at(now);
            if (rec >= 0) {
                const int64_t index = frame_index(rec, o.fps);
                // gsr: on Wayland the screen always counts as changed; with
                // -fm content only frames the compositor sent count.
                const bool due = index > last_index;
                const bool changed = o.fm != FrameRateMode::Content || cap->frames() != last_frame_count;
                if (due && changed)
                    encode_video(rec);
                // The next frame boundary.
                const int64_t next_rec = (frame_index(clock.at(now_ns()), o.fps) + 1) * 1000000000LL / o.fps;
                wake = std::min(wake, now + std::max<int64_t>(0, next_rec - clock.at(now_ns())));
            }
            wake = std::min(wake, cap->next_wake());
        }
        if (sound)
            encode_audio(false);
        const int64_t wait_ns = std::max<int64_t>(0, wake - now_ns());
        if (cap) {
            pollfd p{cap->fd(), POLLIN, 0};
            const int r = poll(&p, 1, int((wait_ns + 999999) / 1000000));
            if (r > 0 && !cap->dispatch()) {
                std::fprintf(stderr, "atrium-record: the Wayland connection broke; saving\n");
                break;
            }
            if (cap->stopped()) {
                std::fprintf(stderr, "atrium-record: the compositor stopped the capture (the screen went away); saving\n");
                break;
            }
        } else {
            timespec ts{0, long(std::min<int64_t>(wait_ns, 10000000))};
            nanosleep(&ts, nullptr);
        }
    }

    // --- the end: what's left in the encoders, then the file's index --------------------
    if (cap && video && !paused) {
        // The picture as it is at the end, so the video lasts until then.
        const int64_t rec = clock.at(now_ns());
        if (rec >= 0 && frame_index(rec, o.fps) > last_index)
            encode_video(rec);
    }
    if (sound) {
        encode_audio(true);
        sound->stop();
        for (size_t i = 0; i < audio.size(); i++) {
            avcodec_send_frame(audio[i].ctx, nullptr);
            out.write(audio[i].ctx, out.audio[i], true, audio[i].packet_pts);
        }
    }
    if (video) {
        avcodec_send_frame(video->context(), nullptr);
        out.write(video->context(), out.video, false, no_audio_pts);
    }
    if (!out.written && out.hybrid)
        std::fprintf(stderr, "atrium-record: warning: not finalizing the video file because it has no video/audio data\n");
    else
        av_write_trailer(out.fmt);
    avio_closep(&out.fmt->pb);
    avformat_free_context(out.fmt);
    if (!is_pipe) {
        std::printf("%s\n", o.file.c_str());
        if (!o.script.empty())
            run_script(o.script, o.file);
    }
    if (o.verbose)
        std::fprintf(stderr, "atrium-record: saved %s\n", o.file.c_str());
    finish(kOk);
}
