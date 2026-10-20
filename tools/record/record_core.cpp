#include "record_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace atrium::record {

const char* const kUsage =
    "usage: atrium-record [-w <monitor|screen|region>] [-c <container_format>] [-s WxH] [-region WxH+X+Y] [-f <fps>]\n"
    "                     [-a <audio_input>] [-q <quality>] [-k h264|hevc|av1|vp8|vp9|hevc_hdr|av1_hdr|hevc_10bit|av1_10bit]\n"
    "                     [-ac aac|opus|flac|mp3] [-ab <bitrate>] [-fm cfr|vfr|content] [-bm auto|qp|vbr|cbr] [-cr limited|full]\n"
    "                     [-tune performance|quality] [-sc <script_path>] [-cursor yes|no] [-keyint <value>] [-encoder gpu|cpu]\n"
    "                     [-fallback-cpu-encoding yes|no] [-o <output_file>] [-ffmpeg-opts <options>] [-ffmpeg-video-opts <options>]\n"
    "                     [-ffmpeg-audio-opts <options>] [-write-first-frame-ts yes|no] [-low-power yes|no] [-v yes|no]\n"
    "                     [-gl-debug yes|no] [-exclude-metadata yes|no] [--version] [-h|--help]\n";

namespace {

enum class Kind { String, Bool, Int, Double, Enum, Later };

struct Spec {
    Kind kind;
    bool list = false;
    int64_t min = 0, max = 0;
    std::vector<std::string> values;  // an enum's
};

// gpu-screen-recorder's options (args_parser.c). "Later" ones are its replay,
// IPC, plugin, portal and streaming parts, not here yet.
const std::map<std::string, Spec>& specs() {
    static const std::map<std::string, Spec> s = {
        {"-w", {Kind::String}},
        {"-c", {Kind::String}},
        {"-f", {Kind::Int, false, 1, 1000}},
        {"-s", {Kind::String}},
        {"-region", {Kind::String}},
        {"-a", {Kind::String, true}},
        {"-q", {Kind::String}},
        {"-o", {Kind::String}},
        {"-k", {Kind::Enum, false, 0, 0, {"auto", "h264", "h265", "hevc", "hevc_hdr", "hevc_10bit", "av1", "av1_hdr", "av1_10bit",
                                         "vp8", "vp9", "h264_vulkan", "hevc_vulkan", "hevc_hdr_vulkan", "hevc_10bit_vulkan",
                                         "av1_vulkan", "av1_hdr_vulkan", "av1_10bit_vulkan"}}},
        {"-ac", {Kind::Enum, false, 0, 0, {"opus", "aac", "flac", "mp3"}}},
        {"-ab", {Kind::Int, false, 0, 50000}},
        {"-oc", {Kind::Bool}},
        {"-fm", {Kind::Enum, false, 0, 0, {"vfr", "cfr", "content"}}},
        {"-bm", {Kind::Enum, false, 0, 0, {"auto", "qp", "cbr", "vbr"}}},
        {"-pixfmt", {Kind::Enum, false, 0, 0, {"yuv420", "yuv444"}}},
        {"-v", {Kind::Bool}},
        {"-gl-debug", {Kind::Bool}},
        {"-df", {Kind::Bool}},
        {"-sc", {Kind::String}},
        {"-cr", {Kind::Enum, false, 0, 0, {"limited", "full"}}},
        {"-tune", {Kind::Enum, false, 0, 0, {"performance", "quality"}}},
        {"-cursor", {Kind::Bool}},
        {"-keyint", {Kind::Double, false, 0, 500}},
        {"-encoder", {Kind::Enum, false, 0, 0, {"gpu", "cpu"}}},
        {"-fallback-cpu-encoding", {Kind::Bool}},
        {"-ffmpeg-opts", {Kind::String}},
        {"-ffmpeg-video-opts", {Kind::String}},
        {"-ffmpeg-audio-opts", {Kind::String}},
        {"-write-first-frame-ts", {Kind::Bool}},
        {"-low-power", {Kind::Bool}},
        {"-exclude-metadata", {Kind::Bool}},
        {"-ro", {Kind::Later}},
        {"-r", {Kind::Later}},
        {"-restart-replay-on-save", {Kind::Later}},
        {"-replay-storage", {Kind::Later}},
        {"-restore-portal-session", {Kind::Later}},
        {"-portal-session-token-filepath", {Kind::Later}},
        {"-p", {Kind::Later, true}},
        {"-auth", {Kind::Later}},
        {"-ipc", {Kind::Later}},
    };
    return s;
}

Parsed fail(std::string why, int code = kFailed) {
    Parsed p;
    p.error = std::move(why);
    p.exit = code;
    return p;
}

bool streaming(const std::string& f) {
    for (const char* scheme : {"http://", "https://", "rtmp://", "rtmps://", "rtsp://", "srt://", "tcp://", "udp://"})
        if (f.starts_with(scheme))
            return true;
    return false;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos)
            return out;
        start = at + 1;
    }
}

} // namespace

Parsed parse_args(const std::vector<std::string>& args) {
    if (args.size() == 1 && (args[0] == "-h" || args[0] == "--help")) {
        Parsed p;
        p.help = true;
        return p;
    }
    if (args.size() == 1 && args[0] == "--version") {
        Parsed p;
        p.version = true;
        return p;
    }
    if (args.empty())
        return fail("no options");

    std::map<std::string, std::vector<std::string>> given;
    for (size_t i = 0; i < args.size(); i += 2) {
        const std::string& key = args[i];
        if (key.starts_with("--"))
            return fail("'" + key + "' isn't here yet (atrium-record has gpu-screen-recorder's recording options only)", kUnsupported);
        const auto it = specs().find(key);
        if (it == specs().end())
            return fail("invalid argument '" + key + "'");
        const Spec& spec = it->second;
        if (given.contains(key) && !spec.list)
            return fail("expected argument '" + key + "' to only be specified once");
        if (i + 1 >= args.size())
            return fail("missing value for argument '" + key + "'");
        const std::string& v = args[i + 1];
        switch (spec.kind) {
        case Kind::Later:
            return fail("'" + key + "' (replay, streaming, plugins, portal and IPC) isn't in atrium-record yet", kUnsupported);
        case Kind::Bool:
            if (v != "yes" && v != "no")
                return fail(key + " should either be 'yes' or 'no', got: '" + v + "'");
            break;
        case Kind::Enum:
            if (std::find(spec.values.begin(), spec.values.end(), v) == spec.values.end()) {
                std::string names;
                for (const std::string& n : spec.values)
                    names += (names.empty() ? "'" : ", '") + n + "'";
                return fail(key + " should either be " + names + ", got: '" + v + "'");
            }
            break;
        case Kind::Int: {
            long long n = 0;
            char tail = 0;
            if (std::sscanf(v.c_str(), "%lld%c", &n, &tail) != 1)
                return fail(key + " argument \"" + v + "\" is not an integer");
            if (n < spec.min || n > spec.max)
                return fail(key + " argument is expected to be from " + std::to_string(spec.min) + " to " + std::to_string(spec.max) + ", got " + v);
            break;
        }
        case Kind::Double: {
            double d = 0;
            if (std::sscanf(v.c_str(), "%lf", &d) != 1)
                return fail(key + " argument \"" + v + "\" is not an floating-point number");
            if (d < double(spec.min) || d > double(spec.max))
                return fail(key + " argument is expected to be from " + std::to_string(spec.min) + " to " + std::to_string(spec.max) + ", got " + v);
            break;
        }
        case Kind::String:
            break;
        }
        given[key].push_back(v);
    }

    auto has = [&](const char* k) { return given.contains(k); };
    auto str = [&](const char* k) { return has(k) ? given[k][0] : std::string(); };
    auto yes = [&](const char* k, bool fallback) { return has(k) ? given[k][0] == "yes" : fallback; };

    Options o;
    o.source = str("-w");
    o.container = str("-c");
    if (o.container == "mkv")
        o.container = "matroska";
    if (has("-f"))
        o.fps = std::stoi(str("-f"));
    for (const std::string& arg : given["-a"]) {
        AudioTrack track;
        for (const std::string& s : split(arg, '|')) {
            if (s.empty())
                continue;
            if (s.starts_with("name:"))
                track.name = s.substr(5);
            else if (s.starts_with("app:") || s.starts_with("app-inverse:"))
                return fail("-a " + s + ": application audio isn't in atrium-record yet", kUnsupported);
            else
                track.sources.push_back(s.starts_with("device:") ? s.substr(7) : s);
        }
        if (track.sources.empty())
            return fail("-a \"" + arg + "\" has no audio device");
        o.audio.push_back(std::move(track));
    }
    if (has("-k")) {
        std::string k = str("-k");
        if (k.ends_with("_vulkan")) {
            o.codec.vulkan = true;
            k.resize(k.size() - 7);
        }
        if (k.ends_with("_hdr")) {
            o.codec.hdr = o.codec.ten_bit = true;
            k.resize(k.size() - 4);
        } else if (k.ends_with("_10bit")) {
            o.codec.ten_bit = true;
            k.resize(k.size() - 6);
        }
        o.codec.codec = k == "h264" ? Codec::H264 : k == "hevc" || k == "h265" ? Codec::Hevc : k == "av1" ? Codec::Av1
                      : k == "vp8" ? Codec::Vp8 : k == "vp9" ? Codec::Vp9 : Codec::Auto;
    }
    if (has("-ac")) {
        const std::string a = str("-ac");
        o.audio_codec = a == "aac" ? AudioCodec::Aac : a == "mp3" ? AudioCodec::Mp3 : AudioCodec::Opus;
        o.audio_codec_given = true;
        // gpu-screen-recorder: "flac audio codec is temporary disabled, using opus".
    }
    if (has("-ab"))
        o.audio_bitrate = std::stoi(str("-ab"));
    if (has("-fm"))
        o.fm = str("-fm") == "cfr" ? FrameRateMode::Cfr : str("-fm") == "content" ? FrameRateMode::Content : FrameRateMode::Vfr;
    if (has("-bm")) {
        const std::string b = str("-bm");
        o.bm = b == "qp" ? BitrateMode::Qp : b == "cbr" ? BitrateMode::Cbr : b == "vbr" ? BitrateMode::Vbr : BitrateMode::Auto;
    }
    // auto is a constant quantizer (gsr picks vbr only on a Steam Deck).
    if (o.bm == BitrateMode::Auto)
        o.bm = BitrateMode::Qp;
    o.pixfmt = str("-pixfmt") == "yuv444" ? PixelFormat::Yuv444 : PixelFormat::Yuv420;
    o.full_range = str("-cr") == "full";
    o.tune = str("-tune") == "quality" ? Tune::Quality : Tune::Performance;
    o.cursor = yes("-cursor", true);
    if (has("-keyint"))
        o.keyint = std::stod(str("-keyint"));
    o.cpu = str("-encoder") == "cpu";
    o.fallback_cpu = yes("-fallback-cpu-encoding", false);
    o.script = str("-sc");
    o.ffmpeg_opts = str("-ffmpeg-opts");
    o.ffmpeg_video_opts = str("-ffmpeg-video-opts");
    o.ffmpeg_audio_opts = str("-ffmpeg-audio-opts");
    o.verbose = yes("-v", true);
    o.gl_debug = yes("-gl-debug", false);
    o.low_power = yes("-low-power", false);
    o.write_first_frame_ts = yes("-write-first-frame-ts", false);
    o.exclude_metadata = yes("-exclude-metadata", false);

    if (o.bm == BitrateMode::Cbr) {
        if (!has("-q"))
            return fail("option '-q' is required when using '-bm cbr' option");
        long long kbps = 0;
        char tail = 0;
        if (std::sscanf(str("-q").c_str(), "%lld%c", &kbps, &tail) != 1)
            return fail("-q argument \"" + str("-q") + "\" is not an integer value. When using '-bm cbr' option '-q' is expected to be an integer value");
        if (kbps < 0)
            return fail("-q is expected to be 0 or larger, got " + str("-q"));
        o.bitrate = kbps * 1000;
    } else if (has("-q")) {
        const std::string q = str("-q");
        if (q == "medium")
            o.quality = Quality::Medium;
        else if (q == "high")
            o.quality = Quality::High;
        else if (q == "very_high")
            o.quality = Quality::VeryHigh;
        else if (q == "ultra")
            o.quality = Quality::Ultra;
        else
            return fail("-q should either be 'medium', 'high', 'very_high' or 'ultra', got: '" + q + "'");
    }
    if (has("-s")) {
        Size s;
        if (std::sscanf(str("-s").c_str(), "%dx%d", &s.width, &s.height) != 2)
            return fail("invalid value for option -s '" + str("-s") + "', expected a value in format WxH");
        if (s.width < 0 || s.height < 0)
            return fail("invalid value for option -s '" + str("-s") + "', expected width and height to be greater or equal to 0");
        o.size = s;
    }
    if (has("-region")) {
        Region r;
        if (std::sscanf(str("-region").c_str(), "%dx%d+%d+%d", &r.width, &r.height, &r.x, &r.y) != 4)
            return fail("invalid value for option -region '" + str("-region") + "', expected a value in format WxH+X+Y");
        if (r.width < 0 || r.height < 0)
            return fail("invalid value for option -region '" + str("-region") + "', expected width and height to be greater or equal to 0");
        o.region = r;
    }
    if (o.source == "region" && !o.region)
        return fail("option -region is required when -w region is used");
    if (o.region && o.source != "region")
        return fail("option -region can only be used when -w region is used");
    for (const char* other : {"portal", "focused"})
        if (o.source == other)
            return fail(std::string("-w ") + other + " isn't in atrium-record yet", kUnsupported);
    if (o.source.starts_with("/dev/video") || o.source.find('|') != std::string::npos || o.source.starts_with("window:") ||
        o.source.starts_with("v4l2:"))
        return fail("-w " + o.source + ": cameras, windows and several sources aren't in atrium-record yet", kUnsupported);
    if (o.source.starts_with("monitor:"))
        o.source = o.source.substr(8);

    o.file = str("-o");
    if (o.file.empty() && o.container.empty())
        return fail("option -c is required when not using option -o");
    if (streaming(o.file))
        return fail("streaming to " + o.file + " isn't in atrium-record yet", kUnsupported);
    if (o.source.empty() && o.audio.empty())
        return fail("option -a is required when option -w is not used (audio only recording)");
    return Parsed{o, "", kOk};
}

std::string extension(const std::string& file) {
    const size_t slash = file.rfind('/');
    const size_t dot = file.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    return file.substr(dot + 1);
}

int h264_qp(Quality q) {
    switch (q) {
    case Quality::Medium: return 35;
    case Quality::High: return 30;
    case Quality::VeryHigh: return 25;
    case Quality::Ultra: return 22;
    }
    return 22;
}

int codec_qp(Codec c, Quality q, bool hdr) {
    int qp = h264_qp(q);
    if (c == Codec::Av1 || c == Codec::Vp9)
        qp *= 4;
    else if (c == Codec::Vp8)
        qp *= 2;
    // 8 bit / 10 bit = 80%
    return hdr ? int(float(qp) * (8.0f / 10.0f)) : qp;
}

std::vector<Codec> auto_codecs(const std::string& ext) {
    if (ext == "webm")
        return {Codec::Vp8};
    return {Codec::H264, Codec::Hevc, Codec::Av1};
}

std::optional<Codec> fallback_codec(Codec c) {
    if (c == Codec::Hevc || c == Codec::Av1)
        return Codec::H264;
    return std::nullopt;
}

AudioCodec audio_codec_for(AudioCodec asked, const std::string& ext, bool mixed, std::string* why) {
    (void)mixed;
    AudioCodec c = asked;
    std::string reason;
    if (ext == "mp3") {
        c = AudioCodec::Mp3;
        reason = ".mp3 files only support the mp3 audio codec";
    } else if ((asked == AudioCodec::Aac || asked == AudioCodec::Mp3 || asked == AudioCodec::Flac) && ext == "webm") {
        c = AudioCodec::Opus;
        reason = ".webm files only support the opus audio codec";
    } else if (asked == AudioCodec::Opus && ext != "mp4" && ext != "mkv" && ext != "webm" && ext != "ts") {
        c = AudioCodec::Aac;
        reason = "the opus audio codec is only supported by .mp4, .mkv, .webm and .ts files";
    }
    if (why)
        *why = c != asked ? reason : std::string();
    return c;
}

int audio_frame_size(AudioCodec c) {
    switch (c) {
    case AudioCodec::Aac: return 1024;
    case AudioCodec::Mp3: return 1152;
    case AudioCodec::Opus:
    case AudioCodec::Flac: return 960;
    }
    return 960;
}

int audio_bitrate(AudioCodec c) {
    switch (c) {
    case AudioCodec::Aac: return 160;
    case AudioCodec::Mp3: return 192;
    case AudioCodec::Opus:
    case AudioCodec::Flac: return 128;
    }
    return 128;
}

double audio_delay(AudioCodec c, int fps) {
    const double frame = 1.0 / double(fps);
    const double base = 0.01 + 1.0 / 165.0;
    if (c == AudioCodec::Aac || c == AudioCodec::Mp3)
        return std::max(0.0, (base + 0.008) * 2.0 - frame);
    return std::max(0.0, base - frame);
}

Size fit(Size src, Size limit) {
    if (limit.width <= 0 || limit.height <= 0 || src.width <= 0 || src.height <= 0)
        return src;
    // scale_keep_aspect_ratio: as large as fits, either way.
    const double scale = std::min(double(limit.width) / src.width, double(limit.height) / src.height);
    return {std::max(1, int(std::lround(src.width * scale))), std::max(1, int(std::lround(src.height * scale)))};
}

Size encoder_size(Size video, bool hardware) {
    Size s{(video.width + 1) & ~1, (video.height + 1) & ~1};
    if (hardware) {
        s.width = std::max(s.width, 128);
        s.height = std::max(s.height, 128);
    }
    return s;
}

int64_t frame_index(int64_t clock_ns, int fps) {
    return clock_ns <= 0 ? 0 : clock_ns * fps / 1000000000;
}

int64_t pts_us(int64_t now_ns, int64_t start_ns) {
    return now_ns > start_ns ? (now_ns - start_ns) / 1000 : 0;
}

namespace {

// Rows Y, U, V of an RGB → YUV matrix (with the offsets as a 4th column),
// laid out column-major for GLSL.
struct Matrix {
    float m[16];
};

// gsr's BT.709 weights don't sum to 1 (its luma gain makes up the rest), so
// green's is given, not derived.
Matrix build(double kr, double kg, double kb, bool full) {
    // Limited range scales chroma by 219/255 too, as gsr's matrices do.
    const double ys = full ? 1.0 : 219.0 / 255.0, cs = ys;
    const double yo = full ? 0.0 : 16.0 / 255.0;
    const double y[3] = {kr * ys, kg * ys, kb * ys};
    const double u[3] = {-kr / (2 * (1 - kb)) * cs, -kg / (2 * (1 - kb)) * cs, 0.5 * cs};
    const double v[3] = {0.5 * cs, -kg / (2 * (1 - kr)) * cs, -kb / (2 * (1 - kr)) * cs};
    Matrix out{};
    for (int col = 0; col < 3; col++) {
        out.m[col * 4 + 0] = float(y[col]);
        out.m[col * 4 + 1] = float(u[col]);
        out.m[col * 4 + 2] = float(v[col]);
        out.m[col * 4 + 3] = 0;
    }
    out.m[12] = float(yo);
    out.m[13] = 0.5f;
    out.m[14] = 0.5f;
    out.m[15] = 1;
    return out;
}

} // namespace

const float* rgb_to_yuv(bool bt2020, bool full_range) {
    // BT.709 by gsr's weights (0.2110/0.7110/0.0710 full; 0.21/0.71/0.07
    // limited), BT.2020 (0.2627/0.0593) for 10-bit.
    static const Matrix m709_full = build(0.2110, 0.7110, 0.0710, true);
    static const Matrix m709_limited = build(0.2100, 0.7100, 0.0700, false);
    static const Matrix m2020_full = build(0.2627, 0.6780, 0.0593, true);
    static const Matrix m2020_limited = build(0.2627, 0.6780, 0.0593, false);
    if (bt2020)
        return full_range ? m2020_full.m : m2020_limited.m;
    return full_range ? m709_full.m : m709_limited.m;
}

void transform_map(int32_t t, float out[4]) {
    // wlroots' transform matrices (util/matrix.c), row-major a b / c d.
    static constexpr float rows[8][4] = {
        {1, 0, 0, 1}, {0, 1, -1, 0}, {-1, 0, 0, -1}, {0, -1, 1, 0},
        {-1, 0, 0, 1}, {0, 1, 1, 0}, {1, 0, 0, -1}, {0, -1, -1, 0},
    };
    const float* r = rows[t >= 0 && t < 8 ? t : 0];
    out[0] = r[0];
    out[1] = r[2];
    out[2] = r[1];
    out[3] = r[3];
}

float luma_gain(bool ten_bit) {
    return ten_bit ? 1.0f : 1.007049345f;
}

} // namespace atrium::record
