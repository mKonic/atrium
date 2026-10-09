#include "record_core.hpp"

#include <cstdlib>

namespace atrium::record {

Parsed parse_args(const std::vector<std::string>& args) {
    Options o;
    bool have_output = false, have_file = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& key = args[i];
        if (i + 1 >= args.size())
            return {std::nullopt, "option '" + key + "' needs a value"};
        const std::string& v = args[++i];
        if (key == "-w") {
            o.output = v;
            have_output = true;
        } else if (key == "-f") {
            o.fps = std::atoi(v.c_str());
            if (o.fps < 1 || o.fps > 1000)
                return {std::nullopt, "-f is a frame rate from 1 to 1000"};
        } else if (key == "-o") {
            o.file = v;
            have_file = true;
        } else if (key == "-a") {
            if (v != "default_output")
                return {std::nullopt, "-a takes default_output (what the speakers play)"};
            o.audio = true;
        } else if (key == "-k") {
            if (v == "h264" || v == "auto")
                o.codec = Codec::H264;
            else if (v == "hevc")
                o.codec = Codec::Hevc;
            else if (v == "av1")
                o.codec = Codec::Av1;
            else
                return {std::nullopt, "-k is h264, hevc or av1"};
        } else if (key == "-q") {
            if (v == "medium")
                o.quality = Quality::Medium;
            else if (v == "high")
                o.quality = Quality::High;
            else if (v == "very_high")
                o.quality = Quality::VeryHigh;
            else if (v == "ultra")
                o.quality = Quality::Ultra;
            else
                return {std::nullopt, "-q is medium, high, very_high or ultra"};
        } else if (key == "-cursor") {
            if (v != "yes" && v != "no")
                return {std::nullopt, "-cursor is yes or no"};
            o.cursor = v == "yes";
        } else {
            return {std::nullopt, "unknown option '" + key + "'"};
        }
    }
    if (!have_output)
        return {std::nullopt, "-w (the screen to record) is required"};
    if (!have_file)
        return {std::nullopt, "-o (the file to write) is required"};
    return {o, ""};
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

int codec_qp(Codec c, Quality q) {
    return c == Codec::Av1 ? h264_qp(q) * 4 : h264_qp(q);
}

std::vector<std::string> encoders(Codec c, bool nvidia) {
    const char* name = c == Codec::H264 ? "h264" : c == Codec::Hevc ? "hevc" : "av1";
    std::vector<std::string> out;
    if (nvidia)
        out.push_back(std::string(name) + "_nvenc");
    out.push_back(std::string(name) + "_vaapi");
    // On the CPU, H.264 whatever was asked: the others are far too slow live.
    out.push_back("libx264");
    return out;
}

int64_t pts_us(int64_t now_ns, int64_t start_ns) {
    return now_ns > start_ns ? (now_ns - start_ns) / 1000 : 0;
}

} // namespace atrium::record
