#pragma once
// The video encoder and what the shader draws into for it, as
// gpu-screen-recorder's encoder/video/*.c and recorder/video_codec.c:
// NVENC from CUDA frames the GL planes are copied into, VA-API surfaces the
// shader draws straight into, Vulkan images shared with GL, or libx264 from
// planes read back to memory.

#include "gpu.hpp"
#include "record_core.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <memory>
#include <string>

namespace atrium::record {

class VideoEncoder {
public:
    virtual ~VideoEncoder();
    // Opens the encoder for `codec` at `size`, the best this GPU has
    // (gsr's codec_select.c, falling back as it does). `exit` is gsr's code
    // when it fails.
    static std::unique_ptr<VideoEncoder> open(Gpu& gpu, const Options& o, const std::string& ext, Size size, bool low_latency,
                                              int& exit, std::string& error);

    AVCodecContext* context() const { return ctx_; }
    const std::string& name() const { return name_; }
    // What it ended up encoding (a fallback to h264 or the CPU is 8-bit SDR).
    bool ten_bit() const { return ten_bit_; }
    bool hdr() const { return hdr_; }
    // The planes the next frame is drawn into.
    virtual const Planes* begin() = 0;
    // That frame, ready for avcodec_send_frame (owned by the caller).
    virtual AVFrame* end() = 0;

protected:
    virtual bool setup(Gpu& gpu, AVCodecContext* ctx, bool ten_bit, std::string& error) = 0;
    AVCodecContext* ctx_ = nullptr;
    std::string name_;
    bool ten_bit_ = false, hdr_ = false;
};

// vulkan.cpp's.
std::unique_ptr<VideoEncoder> make_vulkan_encoder();

} // namespace atrium::record
