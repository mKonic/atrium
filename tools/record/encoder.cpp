#include "encoder.hpp"

#include "cuda.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <dlfcn.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#ifndef GL_R16_EXT
#define GL_R16_EXT 0x822A
#define GL_RG16_EXT 0x822C
#endif

namespace atrium::record {

namespace cuda {

const Api* load() {
    static Api api{};
    static bool tried = false, ok = false;
    if (tried)
        return ok ? &api : nullptr;
    tried = true;
    void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return nullptr;
#define LOAD(name)                                                              \
    api.name = reinterpret_cast<decltype(api.name)>(dlsym(lib, #name));          \
    if (!api.name)                                                              \
        return nullptr;
    LOAD(cuInit)
    LOAD(cuDeviceGetByPCIBusId)
    LOAD(cuDeviceGet)
    LOAD(cuCtxCreate_v2)
    LOAD(cuCtxDestroy_v2)
    LOAD(cuCtxPushCurrent_v2)
    LOAD(cuCtxPopCurrent_v2)
    LOAD(cuGetErrorString)
    LOAD(cuGraphicsGLRegisterImage)
    LOAD(cuGraphicsUnregisterResource)
    LOAD(cuGraphicsMapResources)
    LOAD(cuGraphicsUnmapResources)
    LOAD(cuGraphicsSubResourceGetMappedArray)
    LOAD(cuMemcpy2DAsync_v2)
    LOAD(cuStreamSynchronize)
#undef LOAD
    ok = api.cuInit(0) == 0;
    return ok ? &api : nullptr;
}

const char* error(CUresult r) {
    const char* s = nullptr;
    if (const Api* a = load())
        a->cuGetErrorString(r, &s);
    return s ? s : "unknown CUDA error";
}

} // namespace cuda

namespace {

std::string av_error(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

enum class Backend { Cuda, Vaapi, Vulkan, Cpu };

const char* codec_name(Codec c) {
    switch (c) {
    case Codec::H264: return "h264";
    case Codec::Hevc: return "hevc";
    case Codec::Av1: return "av1";
    case Codec::Vp8: return "vp8";
    case Codec::Vp9: return "vp9";
    case Codec::Auto: break;
    }
    return "h264";
}

std::string encoder_name(Codec c, Backend b) {
    switch (b) {
    case Backend::Cuda: return std::string(codec_name(c)) + "_nvenc";
    case Backend::Vaapi: return std::string(codec_name(c)) + "_vaapi";
    case Backend::Vulkan: return std::string(codec_name(c)) + "_vulkan";
    case Backend::Cpu: return "libx264";
    }
    return {};
}

// --- NVENC: CUDA frames ------------------------------------------------------------------

class CudaEncoder : public VideoEncoder {
public:
    ~CudaEncoder() override {
        if (api_ && cu_) {
            api_->cuCtxPushCurrent_v2(cu_);
            for (CUgraphicsResource r : resources_)
                if (r)
                    api_->cuGraphicsUnregisterResource(r);
            CUcontext ignored;
            api_->cuCtxPopCurrent_v2(&ignored);
        }
        // The CUDA context goes with the device (FFmpeg frees it).
    }
    const Planes* begin() override { return &planes_; }
    AVFrame* end() override {
        AVFrame* f = av_frame_alloc();
        if (av_hwframe_get_buffer(ctx_->hw_frames_ctx, f, 0) < 0) {
            av_frame_free(&f);
            return nullptr;
        }
        api_->cuCtxPushCurrent_v2(cu_);
        // Mapping waits for GL's drawing into the planes.
        bool ok = api_->cuGraphicsMapResources(2, resources_, nullptr) == 0;
        for (int i = 0; i < 2 && ok; i++) {
            CUarray array = nullptr;
            ok = api_->cuGraphicsSubResourceGetMappedArray(&array, resources_[i], 0, 0) == 0;
            cuda::Memcpy2D m{};
            m.srcMemoryType = cuda::kMemoryArray;
            m.srcArray = array;
            m.dstMemoryType = cuda::kMemoryDevice;
            m.dstDevice = CUdeviceptr(reinterpret_cast<uintptr_t>(f->data[i]));
            m.dstPitch = size_t(f->linesize[i]);
            // Y: a byte (two for P010) a pixel; UV: two at half width.
            m.WidthInBytes = size_t(planes_.width[0]) * (ten_bit_ ? 2 : 1);
            m.Height = size_t(planes_.height[i]);
            if (ok)
                ok = api_->cuMemcpy2DAsync_v2(&m, stream_) == 0;
        }
        api_->cuStreamSynchronize(stream_);
        api_->cuGraphicsUnmapResources(2, resources_, nullptr);
        CUcontext ignored;
        api_->cuCtxPopCurrent_v2(&ignored);
        if (!ok)
            av_frame_free(&f);
        return f;
    }

protected:
    bool setup(Gpu& gpu, AVCodecContext* ctx, bool ten_bit, std::string& error) override {
        ten_bit_ = ten_bit;
        api_ = cuda::load();
        if (!api_) {
            error = "no CUDA (libcuda.so.1)";
            return false;
        }
        // The CUDA device of the GPU the frames are on, by its PCI address.
        CUdevice dev = 0;
        bool found = false;
        if (gpu_bus_.size())
            found = api_->cuDeviceGetByPCIBusId(&dev, gpu_bus_.c_str()) == 0;
        if (!found && api_->cuDeviceGet(&dev, 0) != 0) {
            error = "no CUDA device";
            return false;
        }
        if (CUresult r = api_->cuCtxCreate_v2(&cu_, cuda::kCtxSchedAuto, dev); r != 0) {
            error = std::string("CUDA context: ") + cuda::error(r);
            return false;
        }
        CUcontext ignored;
        api_->cuCtxPopCurrent_v2(&ignored);
        AVBufferRef* device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
        auto* hw = static_cast<AVCUDADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
        hw->cuda_ctx = cu_;
        if (av_hwdevice_ctx_init(device) < 0) {
            av_buffer_unref(&device);
            error = "FFmpeg wouldn't take the CUDA context";
            return false;
        }
        stream_ = hw->stream;
        AVBufferRef* frames = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        fc->format = AV_PIX_FMT_CUDA;
        fc->sw_format = ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        fc->width = ctx->width;
        fc->height = ctx->height;
        if (av_hwframe_ctx_init(frames) < 0) {
            av_buffer_unref(&frames);
            error = "CUDA frames";
            return false;
        }
        ctx->pix_fmt = AV_PIX_FMT_CUDA;
        ctx->hw_frames_ctx = frames;
        // gsr's nvenc.c: two GL textures (R8 + RG8, or 16-bit) registered with CUDA.
        const GLenum formats[2] = {ten_bit ? GLenum(GL_R16_EXT) : GLenum(GL_R8), ten_bit ? GLenum(GL_RG16_EXT) : GLenum(GL_RG8)};
        api_->cuCtxPushCurrent_v2(cu_);
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            planes_.width[i] = ctx->width / (i + 1);
            planes_.height[i] = ctx->height / (i + 1);
            GLuint tex = 0;
            ok = gpu.make_plane(formats[i], planes_.width[i], planes_.height[i], tex, planes_.fbo[i]);
            if (ok) {
                CUresult r = api_->cuGraphicsGLRegisterImage(&resources_[i], tex, GL_TEXTURE_2D, cuda::kRegisterReadOnly);
                if (r != 0) {
                    error = std::string("CUDA wouldn't take the GL texture: ") + cuda::error(r);
                    ok = false;
                }
            } else {
                error = "GL planes for CUDA";
            }
        }
        api_->cuCtxPopCurrent_v2(&ignored);
        if (ok)
            gpu.clear(planes_);
        return ok;
    }

public:
    std::string gpu_bus_;

private:
    const cuda::Api* api_ = nullptr;
    CUcontext cu_ = nullptr;
    CUstream stream_ = nullptr;
    CUgraphicsResource resources_[2] = {};
    Planes planes_;
    bool ten_bit_ = false;
};

// --- VA-API: the shader draws into the surfaces ------------------------------------------

class VaapiEncoder : public VideoEncoder {
public:
    ~VaapiEncoder() override {
        for (auto& [id, s] : surfaces_)
            for (int i = 0; i < 2; i++) {
                glDeleteFramebuffers(1, &s.planes.fbo[i]);
                glDeleteTextures(1, &s.texture[i]);
                if (s.image[i] != EGL_NO_IMAGE_KHR)
                    gpu_->egl_destroy_image(gpu_->display(), s.image[i]);
            }
        av_frame_free(&current_);
    }
    const Planes* begin() override {
        av_frame_free(&current_);
        current_ = av_frame_alloc();
        if (av_hwframe_get_buffer(ctx_->hw_frames_ctx, current_, 0) < 0)
            return nullptr;
        const auto id = uintptr_t(current_->data[3]);
        if (auto it = surfaces_.find(id); it != surfaces_.end())
            return &it->second.planes;
        // A surface not seen yet: its planes (gsr's vaapi.c exports them as
        // separate layers) as GL textures.
        AVFrame* drm = av_frame_alloc();
        drm->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(drm, current_, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE) < 0) {
            av_frame_free(&drm);
            std::fprintf(stderr, "atrium-record: a VA-API surface couldn't be exported\n");
            return nullptr;
        }
        const auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(drm->data[0]);
        Surface s;
        bool ok = desc->nb_layers >= 2 || (desc->nb_layers == 1 && desc->layers[0].nb_planes >= 2);
        for (int i = 0; i < 2 && ok; i++) {
            // One layer a plane, or one layer with both.
            const AVDRMLayerDescriptor& layer = desc->nb_layers >= 2 ? desc->layers[i] : desc->layers[0];
            const AVDRMPlaneDescriptor& plane = desc->nb_layers >= 2 ? layer.planes[0] : layer.planes[i];
            const AVDRMObjectDescriptor& obj = desc->objects[plane.object_index];
            const uint32_t fourcc = i == 0 ? (ten_bit_ ? DRM_FORMAT_R16 : DRM_FORMAT_R8)
                                           : (ten_bit_ ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88);
            s.planes.width[i] = ctx_->width / (i + 1);
            s.planes.height[i] = ctx_->height / (i + 1);
            ok = gpu_->import_plane(obj.fd, uint32_t(plane.offset), uint32_t(plane.pitch), obj.format_modifier, fourcc,
                                    s.planes.width[i], s.planes.height[i], s.texture[i], s.planes.fbo[i], s.image[i]);
        }
        av_frame_free(&drm);
        if (!ok) {
            std::fprintf(stderr, "atrium-record: a VA-API surface couldn't be drawn into\n");
            return nullptr;
        }
        auto& stored = surfaces_[id] = s;
        gpu_->clear(stored.planes);
        return &stored.planes;
    }
    AVFrame* end() override {
        // The encoder reads the surface on its own engine: drawing must be done.
        glFinish();
        AVFrame* f = current_;
        current_ = nullptr;
        return f;
    }

protected:
    bool setup(Gpu& gpu, AVCodecContext* ctx, bool ten_bit, std::string& error) override {
        gpu_ = &gpu;
        ten_bit_ = ten_bit;
        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, render_node_.c_str(), nullptr, 0) < 0) {
            error = "no VA-API on " + render_node_;
            return false;
        }
        AVBufferRef* frames = av_hwframe_ctx_alloc(device);
        av_buffer_unref(&device);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        fc->width = ctx->width;
        fc->height = ctx->height;
        if (av_hwframe_ctx_init(frames) < 0) {
            av_buffer_unref(&frames);
            error = "VA-API surfaces";
            return false;
        }
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
        ctx->hw_frames_ctx = frames;
        return true;
    }

public:
    std::string render_node_;

private:
    struct Surface {
        Planes planes;
        GLuint texture[2] = {};
        EGLImage image[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    };
    Gpu* gpu_ = nullptr;
    bool ten_bit_ = false;
    std::map<uintptr_t, Surface> surfaces_;
    AVFrame* current_ = nullptr;
};

// --- CPU: libx264 from planes read back ---------------------------------------------------

class CpuEncoder : public VideoEncoder {
public:
    const Planes* begin() override { return &planes_; }
    AVFrame* end() override {
        AVFrame* f = av_frame_alloc();
        f->format = AV_PIX_FMT_NV12;
        f->width = ctx_->width;
        f->height = ctx_->height;
        if (av_frame_get_buffer(f, 0) < 0) {
            av_frame_free(&f);
            return nullptr;
        }
        // gsr's software.c: the GL planes read back into the frame.
        for (int i = 0; i < 2; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, planes_.fbo[i]);
            GLint format = 0, type = 0;
            glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &format);
            glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &type);
            const int w = planes_.width[i], h = planes_.height[i], channels = i + 1;
            if (type == GL_UNSIGNED_BYTE && format == (i == 0 ? GL_RED : GL_RG)) {
                glPixelStorei(GL_PACK_ROW_LENGTH, f->linesize[i] / channels);
                glReadPixels(0, 0, w, h, GLenum(format), GL_UNSIGNED_BYTE, f->data[i]);
                glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            } else {
                // Only RGBA is promised.
                rgba_.resize(size_t(w) * size_t(h) * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
                for (int y = 0; y < h; y++) {
                    uint8_t* row = f->data[i] + size_t(y) * size_t(f->linesize[i]);
                    const uint8_t* src = rgba_.data() + size_t(y) * size_t(w) * 4;
                    for (int x = 0; x < w; x++)
                        for (int c = 0; c < channels; c++)
                            row[x * channels + c] = src[x * 4 + c];
                }
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return f;
    }

protected:
    bool setup(Gpu& gpu, AVCodecContext* ctx, bool, std::string& error) override {
        ctx->pix_fmt = AV_PIX_FMT_NV12;
        for (int i = 0; i < 2; i++) {
            planes_.width[i] = ctx->width / (i + 1);
            planes_.height[i] = ctx->height / (i + 1);
            GLuint tex = 0;
            if (!gpu.make_plane(i == 0 ? GL_R8 : GL_RG8, planes_.width[i], planes_.height[i], tex, planes_.fbo[i])) {
                error = "GL planes";
                return false;
            }
        }
        gpu.clear(planes_);
        return true;
    }

private:
    Planes planes_;
    std::vector<uint8_t> rgba_;
};

// The PCI address CUDA names a GPU by ("0000:01:00.0").
std::string pci_bus_id(const std::string& node) {
    int fd = ::open(node.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    drmDevice* d = nullptr;
    std::string out;
    if (drmGetDevice2(fd, 0, &d) == 0 && d && d->bustype == DRM_BUS_PCI) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%04x:%02x:%02x.%x", d->businfo.pci->domain, d->businfo.pci->bus, d->businfo.pci->dev,
                      d->businfo.pci->func);
        out = buf;
    }
    if (d)
        drmFreeDevice(&d);
    close(fd);
    return out;
}

// gsr's create_video_codec_context and open_video_hardware/_software.
AVCodecContext* make_context(const AVCodec* codec, const Options& o, Codec c, Size size, bool low_latency, Backend b, bool hdr) {
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = size.width;
    ctx->height = size.height;
    ctx->time_base = {1, o.fm == FrameRateMode::Cfr ? o.fps : AV_TIME_BASE};
    ctx->framerate = {o.fps, 1};
    if (low_latency) {
        ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    ctx->gop_size = int(o.fps * o.keyint);
    ctx->max_b_frames = o.tune == Tune::Quality ? 2 : 0;
    ctx->color_range = o.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (hdr) {
        ctx->color_primaries = AVCOL_PRI_BT2020;
        ctx->color_trc = AVCOL_TRC_SMPTE2084;
        ctx->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->colorspace = AVCOL_SPC_BT709;
    }
    const int qp = codec_qp(c, o.quality, hdr);
    if (o.bm == BitrateMode::Cbr) {
        ctx->bit_rate = o.bitrate;
        ctx->rc_max_rate = ctx->bit_rate;
        ctx->rc_buffer_size = int(ctx->bit_rate);
        ctx->rc_initial_buffer_occupancy = 0;
    } else if (o.bm == BitrateMode::Vbr) {
        ctx->qmin = ctx->qmax = qp;
        ctx->bit_rate = 100000;
        ctx->rc_max_rate = ctx->bit_rate;
        ctx->rc_buffer_size = int(ctx->bit_rate);
        ctx->rc_initial_buffer_occupancy = int(ctx->bit_rate);
    }
    if (b == Backend::Vaapi && o.bm != BitrateMode::Cbr)
        ctx->global_quality = int(float(codec_qp(c, o.quality, false)) * (hdr ? 8.0f / 10.0f * 0.7f : 1.0f));
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    return ctx;
}

AVDictionary* encoder_options(const Options& o, Codec c, Backend b, bool ten_bit, bool hdr, bool low_power, bool livestream) {
    AVDictionary* d = nullptr;
    const int qp = codec_qp(c, o.quality, hdr);
    if (b == Backend::Cpu) {
        if (o.bm == BitrateMode::Qp)
            av_dict_set_int(&d, "qp", qp, 0);
        av_dict_set(&d, "preset", "veryfast", 0);
        av_dict_set(&d, "tune", "film", 0);
        av_dict_set_int(&d, "forced-idr", 1, 0);
        av_dict_set(&d, "coder", "cabac", 0);
        av_dict_set(&d, "strict", "experimental", 0);
        if (!o.ffmpeg_video_opts.empty())
            av_dict_parse_string(&d, o.ffmpeg_video_opts.c_str(), "=", ";", 0);
        return d;
    }
    if (o.bm == BitrateMode::Qp)
        av_dict_set_int(&d, "qp", qp, 0);
    const bool vulkan = b == Backend::Vulkan, nv = b == Backend::Cuda;
    const char* rc = o.bm == BitrateMode::Qp ? (vulkan ? "cqp" : nv ? "constqp" : "CQP")
                   : o.bm == BitrateMode::Vbr ? (vulkan ? "vbr" : nv ? "vbr" : "VBR")
                                              : (vulkan ? "cbr" : nv ? "cbr" : "CBR");
    av_dict_set(&d, nv ? "rc" : "rc_mode", rc, 0);
    // Profiles (dict_set_profile, FFmpeg ≥ 61.17: numbers, NVENC's own).
    if (c == Codec::H264) {
        av_dict_set_int(&d, "profile", nv ? 2 : AV_PROFILE_H264_HIGH, 0);
    } else if (c == Codec::Av1) {
        if (nv) {
            if (ten_bit)
                av_dict_set_int(&d, "highbitdepth", 1, 0);
        } else {
            av_dict_set_int(&d, "profile", AV_PROFILE_AV1_MAIN, 0);
        }
    } else if (c == Codec::Hevc) {
        av_dict_set_int(&d, "profile", ten_bit ? (nv ? 1 : AV_PROFILE_HEVC_MAIN_10) : (nv ? 0 : AV_PROFILE_HEVC_MAIN), 0);
    }
    if (vulkan) {
        av_dict_set_int(&d, "async_depth", 3, 0);
        av_dict_set(&d, "tune", "ll", 0);
        av_dict_set(&d, "usage", livestream ? "stream" : "record", 0);
        av_dict_set(&d, "content", "rendered", 0);
        if (c == Codec::Av1)
            av_dict_set(&d, "tier", "main", 0);
        else if (c == Codec::Hevc && hdr)
            av_dict_set(&d, "sei", "hdr", 0);
    } else if (nv) {
        av_dict_set(&d, "tune", "ll", 0);
        av_dict_set_int(&d, "forced-idr", 1, 0);
        if (o.tune == Tune::Quality) {
            av_dict_set(&d, "multipass", "fullres", 0);
            av_dict_set(&d, "preset", "p6", 0);
            av_dict_set_int(&d, "rc-lookahead", 0, 0);
        }
        if (c == Codec::Av1)
            av_dict_set(&d, "rgb_mode", o.pixfmt == PixelFormat::Yuv444 ? "yuv444" : "yuv420", 0);
    } else {
        if (low_power)
            av_dict_set_int(&d, "low_power", 1, 0);
        av_dict_set_int(&d, "async_depth", 3, 0);
        if (c == Codec::Av1)
            av_dict_set(&d, "tier", "main", 0);
        else if (c == Codec::Hevc && hdr)
            av_dict_set(&d, "sei", "hdr", 0);
    }
    if (c == Codec::H264)
        av_dict_set(&d, "coder", "cabac", 0);
    av_dict_set(&d, "strict", "experimental", 0);
    if (!o.ffmpeg_video_opts.empty())
        av_dict_parse_string(&d, o.ffmpeg_video_opts.c_str(), "=", ";", 0);
    return d;
}

std::unique_ptr<VideoEncoder> make(Backend b, const std::string& node) {
    switch (b) {
    case Backend::Cuda: {
        auto e = std::make_unique<CudaEncoder>();
        e->gpu_bus_ = pci_bus_id(node);
        return e;
    }
    case Backend::Vaapi: {
        auto e = std::make_unique<VaapiEncoder>();
        e->render_node_ = node;
        return e;
    }
    case Backend::Vulkan:
        return make_vulkan_encoder();
    case Backend::Cpu:
        return std::make_unique<CpuEncoder>();
    }
    return nullptr;
}

} // namespace

VideoEncoder::~VideoEncoder() {
    avcodec_free_context(&ctx_);
}

std::unique_ptr<VideoEncoder> VideoEncoder::open(Gpu& gpu, const Options& o, const std::string& ext, Size video, bool low_latency,
                                                 int& exit, std::string& error) {
    const std::string& node = gpu.node();
    Backend backend = o.cpu ? Backend::Cpu : o.codec.vulkan ? Backend::Vulkan : gpu.nvidia() ? Backend::Cuda : Backend::Vaapi;
    bool ten_bit = o.codec.ten_bit, hdr = o.codec.hdr;

    // One attempt: that codec on that backend.
    auto attempt = [&](Codec c, Backend b, std::string& why) -> std::unique_ptr<VideoEncoder> {
        const std::string name = encoder_name(c, b);
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (!codec) {
            why = "FFmpeg has no " + name;
            return nullptr;
        }
        const bool tb = b == Backend::Cpu ? false : ten_bit;
        const bool h = b == Backend::Cpu ? false : hdr;
        auto enc = make(b, node);
        if (!enc) {
            why = name + " isn't in atrium-record";
            return nullptr;
        }
        Size size = encoder_size(video, b != Backend::Cpu);
        if (b == Backend::Cpu)
            size.width = (size.width + 3) & ~3;  // gsr's software.c: width a multiple of 4
        AVCodecContext* ctx = make_context(codec, o, c, size, low_latency, b, h);
        if (!enc->setup(gpu, ctx, tb, why)) {
            avcodec_free_context(&ctx);
            return nullptr;
        }
        AVDictionary* opts = encoder_options(o, c, b, tb, h, o.low_power, false);
        const int err = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (err < 0) {
            why = name + " wouldn't open at " + std::to_string(size.width) + "x" + std::to_string(size.height) + " (" +
                  av_error(err) + ")";
            avcodec_free_context(&ctx);
            return nullptr;
        }
        enc->ctx_ = ctx;
        enc->name_ = name;
        enc->ten_bit_ = tb;
        enc->hdr_ = h;
        return enc;
    };

    auto cpu = [&](std::string& why) {
        std::fprintf(stderr, "atrium-record: warning: gpu encoding is not available, trying cpu encoding instead because "
                             "-fallback-cpu-encoding is enabled\n");
        return attempt(Codec::H264, Backend::Cpu, why);
    };

    std::string why;
    if (backend == Backend::Cpu) {
        if (o.codec.codec != Codec::Auto && o.codec.codec != Codec::H264)
            std::fprintf(stderr, "atrium-record: warning: cpu encoding is h264 only\n");
        if (auto e = attempt(Codec::H264, Backend::Cpu, why))
            return e;
        error = why;
        exit = kCodecUnsupported;
        return nullptr;
    }

    if (o.codec.codec == Codec::Auto) {
        // gsr: webm gets vp8; else h264, hevc, av1, the first the GPU encodes at this size.
        for (Codec c : auto_codecs(ext)) {
            if (auto e = attempt(c, backend, why)) {
                std::fprintf(stderr, "atrium-record: using %s encoder because a codec was not specified\n", codec_name(c));
                return e;
            }
            if (o.verbose)
                std::fprintf(stderr, "atrium-record: %s\n", why.c_str());
        }
        if (o.fallback_cpu)
            if (auto e = cpu(why))
                return e;
        error = "no video encoder was specified and neither h264, hevc nor av1 are supported here (" + why +
                "); -fallback-cpu-encoding yes encodes on the cpu";
        exit = kNoVideoCodec;
        return nullptr;
    }

    Codec c = o.codec.codec;
    for (;;) {
        if (auto e = attempt(c, backend, why))
            return e;
        std::fprintf(stderr, "atrium-record: %s\n", why.c_str());
        if (backend == Backend::Vulkan) {
            // gsr: *_vulkan falls back to the same codec without Vulkan.
            std::fprintf(stderr, "atrium-record: warning: selected video codec %s_vulkan is not supported, trying %s instead\n",
                         codec_name(c), codec_name(c));
            backend = gpu.nvidia() ? Backend::Cuda : Backend::Vaapi;
            continue;
        }
        if (auto next = fallback_codec(c)) {
            std::fprintf(stderr, "atrium-record: warning: selected video codec %s is not supported, trying %s instead\n",
                         codec_name(c), codec_name(*next));
            c = *next;
            ten_bit = hdr = false;
            continue;
        }
        break;
    }
    if (c == Codec::H264 && o.fallback_cpu)
        if (auto e = cpu(why))
            return e;
    error = std::string("your gpu does not support '") + codec_name(c) + "' video codec (" + why +
            "). If your GPU doesn't encode video, -fallback-cpu-encoding yes encodes on the cpu";
    exit = kCodecUnsupported;
    return nullptr;
}

} // namespace atrium::record
