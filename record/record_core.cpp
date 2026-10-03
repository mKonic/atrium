#include "record_core.hpp"

#include <algorithm>
#include <charconv>

namespace atrium::record {

std::optional<Options> parse_args(const std::vector<std::string>& args, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<Options> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-a") {
            o.audio = true;
        } else if (a == "-o" || a == "-f") {
            if (i + 1 >= args.size())
                return fail(a + " needs a value");
            const std::string& v = args[++i];
            if (a == "-o") {
                o.output = v;
            } else {
                int fps = 0;
                auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), fps);
                if (ec != std::errc() || end != v.data() + v.size() || fps < 1 || fps > 1000)
                    return fail("-f takes frames a second, 1 to 1000");
                o.fps = fps;
            }
        } else if (a.starts_with("-") && a.size() > 1) {
            return fail("unknown option " + a);
        } else if (o.file.empty()) {
            o.file = a;
        } else {
            return fail("one file only");
        }
    }
    if (o.file.empty())
        return fail("which file?");
    return o;
}

std::vector<std::string> encoders_to_try(const std::vector<std::string>& available) {
    std::vector<std::string> out;
    for (const char* name : {"h264_nvenc", "h264_vaapi", "libx264"})
        if (std::ranges::find(available, name) != available.end())
            out.push_back(name);
    return out;
}

bool keep_frame(int64_t us, int64_t last_us, int fps) {
    if (last_us < 0 || fps <= 0)
        return true;
    return us - last_us >= 1'000'000 / fps * 3 / 4;
}

int64_t Timeline::video_us(int64_t ns) {
    if (start_ns_ < 0)
        start_ns_ = ns;
    // Frames keep their order even if a stamp goes backwards.
    last_us_ = std::max((ns - start_ns_) / 1000, last_us_ + 1);
    return last_us_;
}

std::optional<int64_t> Timeline::audio_us(int64_t ns) const {
    if (start_ns_ < 0)
        return std::nullopt;
    return (ns - start_ns_) / 1000;
}

} // namespace atrium::record
