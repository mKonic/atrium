// atrium-screenshot: atrium's own grim. Takes the screens (or a part of them,
// or one window) through ext-image-copy-capture and writes a PNG, PPM or
// JPEG, with grim's options, so anything written for grim works with it:
//
//   atrium-screenshot [-s SCALE] [-g "X,Y WxH"] [-o OUTPUT] [-T WINDOW] [-c]
//                     [-t png|ppm|jpeg] [-q QUALITY] [-l LEVEL] [FILE|-]
//
// -T takes a window by its ext-foreign-toplevel identifier, whole even when
// covered. The compositing is grim's (render.c): each screen's picture is
// turned, flipped and scaled into place with pixman.

#include "shot_core.hpp"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <jpeglib.h>
#include <pixman.h>
#include <png.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace atrium::shot;

namespace {

struct Buffer {
    wl_buffer* wl = nullptr;
    void* data = nullptr;
    size_t size = 0;
    uint32_t format = 0;
    int width = 0, height = 0, stride = 0;
    ~Buffer() {
        if (wl)
            wl_buffer_destroy(wl);
        if (data)
            munmap(data, size);
    }
};

// A screen, or the window taken with -T.
struct Source {
    wl_output* output = nullptr;
    zxdg_output_v1* xdg = nullptr;
    std::string name;
    Box logical;                // on the desktop
    int mode_width = 0, mode_height = 0;
    uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
    double logical_scale = 1;
    // The capture.
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    int buffer_width = 0, buffer_height = 0;
    uint32_t shm_format = 0;
    bool has_format = false;
    std::unique_ptr<Buffer> buffer;
    bool done = false, failed = false;
};

struct Toplevel {
    ext_foreign_toplevel_handle_v1* handle = nullptr;
    std::string identifier;
};

struct State {
    wl_shm* shm = nullptr;
    zxdg_output_manager_v1* xdg_outputs = nullptr;
    ext_image_copy_capture_manager_v1* copy = nullptr;
    ext_output_image_capture_source_manager_v1* output_sources = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevel_sources = nullptr;
    ext_foreign_toplevel_list_v1* list = nullptr;
    std::vector<std::unique_ptr<Source>> outputs;
    std::vector<std::unique_ptr<Toplevel>> toplevels;
};

// --- formats (grim's render.c) -------------------------------------------------------

pixman_format_code_t pixman_format(uint32_t f) {
    switch (f) {
    case WL_SHM_FORMAT_ARGB8888: return PIXMAN_a8r8g8b8;
    case WL_SHM_FORMAT_XRGB8888: return PIXMAN_x8r8g8b8;
    case WL_SHM_FORMAT_ABGR8888: return PIXMAN_a8b8g8r8;
    case WL_SHM_FORMAT_XBGR8888: return PIXMAN_x8b8g8r8;
    case WL_SHM_FORMAT_BGRA8888: return PIXMAN_b8g8r8a8;
    case WL_SHM_FORMAT_BGRX8888: return PIXMAN_b8g8r8x8;
    case WL_SHM_FORMAT_RGBA8888: return PIXMAN_r8g8b8a8;
    case WL_SHM_FORMAT_RGBX8888: return PIXMAN_r8g8b8x8;
    case WL_SHM_FORMAT_ARGB2101010: return PIXMAN_a2r10g10b10;
    case WL_SHM_FORMAT_ABGR2101010: return PIXMAN_a2b10g10r10;
    case WL_SHM_FORMAT_XRGB2101010: return PIXMAN_x2r10g10b10;
    case WL_SHM_FORMAT_XBGR2101010: return PIXMAN_x2b10g10r10;
    case WL_SHM_FORMAT_RGB565: return PIXMAN_r5g6b5;
    case WL_SHM_FORMAT_RGB888: return PIXMAN_r8g8b8;
    case WL_SHM_FORMAT_BGR888: return PIXMAN_b8g8r8;
    default: return pixman_format_code_t(0);
    }
}

std::unique_ptr<Buffer> make_buffer(wl_shm* shm, uint32_t format, int width, int height) {
    auto b = std::make_unique<Buffer>();
    b->format = format;
    b->width = width;
    b->height = height;
    b->stride = ((width * PIXMAN_FORMAT_BPP(pixman_format(format)) + 0x1f) >> 5) * 4;
    b->size = size_t(b->stride) * size_t(height);
    const int fd = memfd_create("atrium-screenshot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, off_t(b->size)) < 0)
        return nullptr;
    b->data = mmap(nullptr, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->data == MAP_FAILED) {
        b->data = nullptr;
        close(fd);
        return nullptr;
    }
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, int32_t(b->size));
    b->wl = wl_shm_pool_create_buffer(pool, 0, width, height, b->stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

// --- Wayland -----------------------------------------------------------------------

State* g_state = nullptr;

const ext_image_copy_capture_frame_v1_listener kFrame = {
    .transform = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t t) { static_cast<Source*>(data)->transform = t; },
    .damage = [](void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {},
    .presentation_time = [](void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {},
    .ready = [](void* data, ext_image_copy_capture_frame_v1*) { static_cast<Source*>(data)->done = true; },
    .failed = [](void* data, ext_image_copy_capture_frame_v1*, uint32_t) {
        auto* s = static_cast<Source*>(data);
        s->failed = s->done = true;
    },
};

const ext_image_copy_capture_session_v1_listener kSession = {
    .buffer_size = [](void* data, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto* s = static_cast<Source*>(data);
        s->buffer_width = int(w);
        s->buffer_height = int(h);
    },
    .shm_format = [](void* data, ext_image_copy_capture_session_v1*, uint32_t f) {
        auto* s = static_cast<Source*>(data);
        if (!s->has_format && pixman_format(f)) {
            s->shm_format = f;
            s->has_format = true;
        }
    },
    .dmabuf_device = [](void*, ext_image_copy_capture_session_v1*, wl_array*) {},
    .dmabuf_format = [](void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {},
    .done = [](void* data, ext_image_copy_capture_session_v1* session) {
        auto* s = static_cast<Source*>(data);
        if (s->frame)
            return;
        if (!s->has_format || s->buffer_width <= 0) {
            s->failed = s->done = true;
            return;
        }
        s->buffer = make_buffer(g_state->shm, s->shm_format, s->buffer_width, s->buffer_height);
        if (!s->buffer) {
            s->failed = s->done = true;
            return;
        }
        s->frame = ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(s->frame, &kFrame, s);
        ext_image_copy_capture_frame_v1_attach_buffer(s->frame, s->buffer->wl);
        ext_image_copy_capture_frame_v1_damage_buffer(s->frame, 0, 0, INT32_MAX, INT32_MAX);
        ext_image_copy_capture_frame_v1_capture(s->frame);
    },
    .stopped = [](void* data, ext_image_copy_capture_session_v1*) {
        auto* s = static_cast<Source*>(data);
        if (!s->done)
            s->failed = s->done = true;
    },
};

const wl_output_listener kOutput = {
    .geometry = [](void* data, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t t) {
        static_cast<Source*>(data)->transform = uint32_t(t);
    },
    .mode = [](void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            static_cast<Source*>(data)->mode_width = w;
            static_cast<Source*>(data)->mode_height = h;
        }
    },
    .done = [](void*, wl_output*) {},
    .scale = [](void*, wl_output*, int32_t) {},
    .name = [](void* data, wl_output*, const char* name) { static_cast<Source*>(data)->name = name; },
    .description = [](void*, wl_output*, const char*) {},
};

const zxdg_output_v1_listener kXdgOutput = {
    .logical_position = [](void* data, zxdg_output_v1*, int32_t x, int32_t y) {
        static_cast<Source*>(data)->logical.x = x;
        static_cast<Source*>(data)->logical.y = y;
    },
    .logical_size = [](void* data, zxdg_output_v1*, int32_t w, int32_t h) {
        static_cast<Source*>(data)->logical.width = w;
        static_cast<Source*>(data)->logical.height = h;
    },
    .done = [](void*, zxdg_output_v1*) {},
    .name = [](void*, zxdg_output_v1*, const char*) {},
    .description = [](void*, zxdg_output_v1*, const char*) {},
};

const ext_foreign_toplevel_handle_v1_listener kToplevel = {
    .closed = [](void*, ext_foreign_toplevel_handle_v1*) {},
    .done = [](void*, ext_foreign_toplevel_handle_v1*) {},
    .title = [](void*, ext_foreign_toplevel_handle_v1*, const char*) {},
    .app_id = [](void*, ext_foreign_toplevel_handle_v1*, const char*) {},
    .identifier = [](void* data, ext_foreign_toplevel_handle_v1*, const char* id) {
        static_cast<Toplevel*>(data)->identifier = id;
    },
};

const ext_foreign_toplevel_list_v1_listener kList = {
    .toplevel = [](void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* h) {
        auto t = std::make_unique<Toplevel>();
        t->handle = h;
        ext_foreign_toplevel_handle_v1_add_listener(h, &kToplevel, t.get());
        static_cast<State*>(data)->toplevels.push_back(std::move(t));
    },
    .finished = [](void*, ext_foreign_toplevel_list_v1*) {},
};

const wl_registry_listener kRegistry = {
    .global = [](void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
        auto* st = static_cast<State*>(data);
        auto is = [iface](const wl_interface& i) { return std::strcmp(iface, i.name) == 0; };
        if (is(wl_shm_interface)) {
            st->shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        } else if (is(wl_output_interface)) {
            auto o = std::make_unique<Source>();
            o->output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, std::min(version, 4u)));
            wl_output_add_listener(o->output, &kOutput, o.get());
            st->outputs.push_back(std::move(o));
        } else if (is(zxdg_output_manager_v1_interface)) {
            st->xdg_outputs = static_cast<zxdg_output_manager_v1*>(
                wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, std::min(version, 2u)));
        } else if (is(ext_image_copy_capture_manager_v1_interface)) {
            st->copy = static_cast<ext_image_copy_capture_manager_v1*>(
                wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1));
        } else if (is(ext_output_image_capture_source_manager_v1_interface)) {
            st->output_sources = static_cast<ext_output_image_capture_source_manager_v1*>(
                wl_registry_bind(reg, name, &ext_output_image_capture_source_manager_v1_interface, 1));
        } else if (is(ext_foreign_toplevel_image_capture_source_manager_v1_interface)) {
            st->toplevel_sources = static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
                wl_registry_bind(reg, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1));
        } else if (is(ext_foreign_toplevel_list_v1_interface)) {
            st->list = static_cast<ext_foreign_toplevel_list_v1*>(
                wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, 1));
            ext_foreign_toplevel_list_v1_add_listener(st->list, &kList, st);
        }
    },
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

void start_capture(State& st, Source& s, ext_image_capture_source_v1* source, bool cursor) {
    s.session = ext_image_copy_capture_manager_v1_create_session(
        st.copy, source, cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(s.session, &kSession, &s);
    ext_image_capture_source_v1_destroy(source);
}

// --- compositing (grim's render.c) ----------------------------------------------------------

void transformed_size(uint32_t t, int& w, int& h) {
    if (t & WL_OUTPUT_TRANSFORM_90)
        std::swap(w, h);
}

double rotation(uint32_t t) {
    switch (t & ~uint32_t(WL_OUTPUT_TRANSFORM_FLIPPED)) {
    case WL_OUTPUT_TRANSFORM_90: return M_PI / 2;
    case WL_OUTPUT_TRANSFORM_180: return M_PI;
    case WL_OUTPUT_TRANSFORM_270: return 3 * M_PI / 2;
    default: return 0;
    }
}

Box composite_region(const pixman_f_transform& out2com, int w, int h, bool& aligned) {
    pixman_transform fixed;
    pixman_transform_from_pixman_f_transform(&fixed, &out2com);
    pixman_vector corners[4] = {
        {{0, 0, pixman_fixed_1}},
        {{pixman_int_to_fixed(w), 0, pixman_fixed_1}},
        {{0, pixman_int_to_fixed(h), pixman_fixed_1}},
        {{pixman_int_to_fixed(w), pixman_int_to_fixed(h), pixman_fixed_1}},
    };
    pixman_fixed_t x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;
    for (auto& c : corners) {
        pixman_transform_point(&fixed, &c);
        x0 = std::min(x0, c.vector[0]);
        x1 = std::max(x1, c.vector[0]);
        y0 = std::min(y0, c.vector[1]);
        y1 = std::max(y1, c.vector[1]);
    }
    aligned = !pixman_fixed_frac(x0) && !pixman_fixed_frac(x1) && !pixman_fixed_frac(y0) && !pixman_fixed_frac(y1);
    const int ix0 = pixman_fixed_to_int(pixman_fixed_floor(x0)), ix1 = pixman_fixed_to_int(pixman_fixed_ceil(x1));
    const int iy0 = pixman_fixed_to_int(pixman_fixed_floor(y0)), iy1 = pixman_fixed_to_int(pixman_fixed_ceil(y1));
    return {ix0, iy0, ix1 - ix0, iy1 - iy0};
}

pixman_image_t* render(const std::vector<Source*>& sources, const Box& geometry, double scale) {
    const int width = int(geometry.width * scale), height = int(geometry.height * scale);
    pixman_image_t* common = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, nullptr, 0);
    if (!common) {
        std::fprintf(stderr, "failed to create image with size: %d x %d\n", width, height);
        return nullptr;
    }
    for (Source* s : sources) {
        const Buffer* b = s->buffer.get();
        if (!b)
            continue;
        pixman_image_t* image = pixman_image_create_bits(pixman_format(b->format), b->width, b->height,
                                                         static_cast<uint32_t*>(b->data), b->stride);
        int raw_w = s->mode_width, raw_h = s->mode_height;
        transformed_size(s->transform, raw_w, raw_h);
        const int flip_x = (s->transform & WL_OUTPUT_TRANSFORM_FLIPPED) ? -1 : 1;
        pixman_f_transform out2com;
        pixman_f_transform_init_identity(&out2com);
        pixman_f_transform_translate(&out2com, nullptr, -double(s->mode_width) / 2, -double(s->mode_height) / 2);
        pixman_f_transform_scale(&out2com, nullptr, double(s->logical.width) / raw_w, double(s->logical.height) / raw_h);
        pixman_f_transform_rotate(&out2com, nullptr, std::round(std::cos(rotation(s->transform))),
                                  std::round(std::sin(rotation(s->transform))));
        pixman_f_transform_scale(&out2com, nullptr, flip_x, 1);
        pixman_f_transform_translate(&out2com, nullptr, double(s->logical.width) / 2, double(s->logical.height) / 2);
        pixman_f_transform_translate(&out2com, nullptr, s->logical.x - geometry.x, s->logical.y - geometry.y);
        pixman_f_transform_scale(&out2com, nullptr, scale, scale);
        bool aligned = false;
        const Box dest = composite_region(out2com, b->width, b->height, aligned);
        pixman_f_transform_translate(&out2com, nullptr, -dest.x, -dest.y);
        pixman_f_transform com2out;
        pixman_f_transform_invert(&com2out, &out2com);
        pixman_transform c2o;
        pixman_transform_from_pixman_f_transform(&c2o, &com2out);
        pixman_image_set_transform(image, &c2o);

        const double xs = std::fmax(std::fabs(out2com.m[0][0]), std::fabs(out2com.m[0][1]));
        const double ys = std::fmax(std::fabs(out2com.m[1][0]), std::fabs(out2com.m[1][1]));
        if (xs >= 0.75 && ys >= 0.75) {
            pixman_image_set_filter(image, PIXMAN_FILTER_BILINEAR, nullptr, 0);
        } else {
            // Downscaling: each pixel gathers the region it stands for.
            int n = 0;
            pixman_fixed_t* conv = pixman_filter_create_separable_convolution(&n,
                pixman_double_to_fixed(std::fmax(1., 1. / xs)), pixman_double_to_fixed(std::fmax(1., 1. / ys)),
                PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_IMPULSE, PIXMAN_KERNEL_LANCZOS2, PIXMAN_KERNEL_LANCZOS2, 2, 2);
            pixman_image_set_filter(image, PIXMAN_FILTER_SEPARABLE_CONVOLUTION, conv, n);
            std::free(conv);
        }
        bool overlapping = false;
        for (Source* o : sources)
            if (o != s && intersects(o->logical, s->logical))
                overlapping = true;
        pixman_image_composite32(aligned && !overlapping ? PIXMAN_OP_SRC : PIXMAN_OP_OVER, image, nullptr, common,
                                 0, 0, 0, 0, dest.x, dest.y, dest.width, dest.height);
        pixman_image_unref(image);
    }
    return common;
}

// --- writing (grim's write_*.c) -------------------------------------------------------------

bool write_png(pixman_image_t* image, FILE* f, int level) {
    const int w = pixman_image_get_width(image), h = pixman_image_get_height(image);
    const int stride = pixman_image_get_stride(image);
    const auto* data = reinterpret_cast<const uint8_t*>(pixman_image_get_data(image));
    bool opaque = true;
    for (int y = 0; y < h && opaque; y++)
        for (int x = 0; x < w; x++)
            if ((reinterpret_cast<const uint32_t*>(data + y * stride)[x] >> 24) != 0xff) {
                opaque = false;
                break;
            }
    png_struct* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_info* info = png ? png_create_info_struct(png) : nullptr;
    if (!info)
        return false;
    std::vector<uint8_t> row(size_t(w) * 4);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_init_io(png, f);
    png_set_IHDR(png, info, uint32_t(w), uint32_t(h), 8, opaque ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    png_set_compression_level(png, level);
    png_set_filter(png, 0, level == 0 ? PNG_NO_FILTERS : PNG_ALL_FILTERS);
    for (int y = 0; y < h; y++) {
        const auto* in = reinterpret_cast<const uint32_t*>(data + y * stride);
        uint8_t* out = row.data();
        for (int x = 0; x < w; x++) {
            uint32_t r = (in[x] >> 16) & 0xff, g = (in[x] >> 8) & 0xff, b = in[x] & 0xff, a = in[x] >> 24;
            if (!opaque && a != 0 && a != 255) {  // unpremultiply
                const uint32_t inv = (0xffu << 16) / a;
                r = std::min(0xffu, (r * inv) >> 16);
                g = std::min(0xffu, (g * inv) >> 16);
                b = std::min(0xffu, (b * inv) >> 16);
            }
            *out++ = uint8_t(r);
            *out++ = uint8_t(g);
            *out++ = uint8_t(b);
            if (!opaque)
                *out++ = uint8_t(a);
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return true;
}

bool write_ppm(pixman_image_t* image, FILE* f) {
    const int w = pixman_image_get_width(image), h = pixman_image_get_height(image);
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const uint32_t* px = pixman_image_get_data(image);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = *px++;
            row[size_t(x) * 3] = uint8_t(p >> 16);
            row[size_t(x) * 3 + 1] = uint8_t(p >> 8);
            row[size_t(x) * 3 + 2] = uint8_t(p);
        }
        if (std::fwrite(row.data(), 1, row.size(), f) != row.size())
            return false;
    }
    return true;
}

bool write_jpeg(pixman_image_t* image, FILE* f, int quality) {
    jpeg_compress_struct c{};
    jpeg_error_mgr err{};
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = JDIMENSION(pixman_image_get_width(image));
    c.image_height = JDIMENSION(pixman_image_get_height(image));
    c.in_color_space = JCS_EXT_BGRA;
    c.input_components = 4;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    auto* data = reinterpret_cast<uint8_t*>(pixman_image_get_data(image));
    const int stride = pixman_image_get_stride(image);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = data + size_t(c.next_scanline) * size_t(stride);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return true;
}

const char kUsage[] =
    "Usage: atrium-screenshot [options...] [output-file]\n"
    "\n"
    "  -h              Show help message and quit.\n"
    "  -s <factor>     Set the output image scale factor. Defaults to the\n"
    "                  greatest output scale factor.\n"
    "  -g <geometry>   Set the region to capture (\"X,Y WxH\", - reads it from stdin).\n"
    "  -t png|ppm|jpeg Set the output filetype. Defaults to png.\n"
    "  -q <quality>    Set the JPEG filetype quality 0-100. Defaults to 80.\n"
    "  -l <level>      Set the PNG filetype compression level 0-9. Defaults to 6.\n"
    "  -o <output>     Set the output name to capture.\n"
    "  -T <identifier> Capture the window with this foreign-toplevel identifier.\n"
    "  -c              Include cursors in the screenshot.\n";

} // namespace

int main(int argc, char** argv) {
    double scale = 1;
    bool greatest = true, cursor = false;
    std::optional<Box> geometry;
    std::string output_name, toplevel_id;
    FileType type = FileType::Png;
    int quality = 80, level = 6, opt;
    while ((opt = getopt(argc, argv, "hs:g:t:q:l:o:T:c")) != -1) {
        switch (opt) {
        case 'h': std::fputs(kUsage, stdout); return 0;
        case 's':
            greatest = false;
            scale = std::strtod(optarg, nullptr);
            if (scale <= 0) {
                std::fprintf(stderr, "invalid scale\n");
                return 1;
            }
            break;
        case 'g': {
            std::string text = optarg;
            if (text == "-" && !std::getline(std::cin, text)) {
                std::fprintf(stderr, "failed to read a line from stdin\n");
                return 1;
            }
            geometry = parse_box(text);
            if (!geometry) {
                std::fprintf(stderr, "invalid geometry\n");
                return 1;
            }
            break;
        }
        case 't': {
            const auto t = file_type(optarg);
            if (!t) {
                std::fprintf(stderr, "invalid filetype\n");
                return 1;
            }
            type = *t;
            break;
        }
        case 'q': quality = std::clamp(std::atoi(optarg), 0, 100); break;
        case 'l': level = std::clamp(std::atoi(optarg), 0, 9); break;
        case 'o': output_name = optarg; break;
        case 'T': toplevel_id = optarg; break;
        case 'c': cursor = true; break;
        default: return 1;
        }
    }
    if (optind < argc - 1) {
        std::fputs(kUsage, stderr);
        return 1;
    }

    std::string path = optind < argc ? argv[optind] : "";
    if (path.empty()) {
        std::string user_dirs;
        const char* home = std::getenv("HOME");
        const char* config = std::getenv("XDG_CONFIG_HOME");
        const std::string file = (config && *config ? std::string(config) : std::string(home ? home : "") + "/.config") + "/user-dirs.dirs";
        std::ifstream in(file);
        std::stringstream ss;
        ss << in.rdbuf();
        path = pictures_dir(std::getenv("GRIM_DEFAULT_DIR"), ss.str(), home) + "/" + default_name(type, std::time(nullptr));
    }

    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "failed to create display\n");
        return 1;
    }
    State st;
    g_state = &st;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, &st);
    wl_display_roundtrip(display);
    if (!st.shm || !st.copy) {
        std::fprintf(stderr, "compositor doesn't support ext-image-copy-capture\n");
        return 1;
    }
    if (st.xdg_outputs)
        for (auto& o : st.outputs) {
            o->xdg = zxdg_output_manager_v1_get_xdg_output(st.xdg_outputs, o->output);
            zxdg_output_v1_add_listener(o->xdg, &kXdgOutput, o.get());
        }
    wl_display_roundtrip(display);  // names, places and the windows' identifiers

    std::vector<Source*> taken;
    Source window;
    if (!toplevel_id.empty()) {
        Toplevel* t = nullptr;
        for (auto& c : st.toplevels)
            if (c->identifier == toplevel_id)
                t = c.get();
        if (!t || !st.toplevel_sources) {
            std::fprintf(stderr, "unknown window '%s'\n", toplevel_id.c_str());
            return 1;
        }
        start_capture(st, window, ext_foreign_toplevel_image_capture_source_manager_v1_create_source(st.toplevel_sources, t->handle), cursor);
        taken.push_back(&window);
    } else {
        for (auto& o : st.outputs) {
            if (o->logical.width <= 0) {  // no xdg-output: as grim guesses it
                o->logical = {0, 0, o->mode_width, o->mode_height};
                transformed_size(o->transform, o->logical.width, o->logical.height);
            }
            int w = o->mode_width, h = o->mode_height;
            transformed_size(o->transform, w, h);
            o->logical_scale = o->logical.width > 0 ? double(w) / o->logical.width : 1;
        }
        if (!output_name.empty()) {
            for (auto& o : st.outputs)
                if (o->name == output_name)
                    geometry = o->logical;
            if (!geometry) {
                std::fprintf(stderr, "unknown output '%s'\n", output_name.c_str());
                return 1;
            }
        }
        std::vector<double> scales;
        for (auto& o : st.outputs) {
            if (geometry && !intersects(*geometry, o->logical))
                continue;
            scales.push_back(o->logical_scale);
            start_capture(st, *o, ext_output_image_capture_source_manager_v1_create_source(st.output_sources, o->output), cursor);
            taken.push_back(o.get());
        }
        if (taken.empty()) {
            std::fprintf(stderr, "supplied geometry did not intersect with any outputs\n");
            return 1;
        }
        if (greatest)
            scale = greatest_scale(scales);
    }

    auto all_done = [&] {
        for (Source* s : taken)
            if (!s->done)
                return false;
        return true;
    };
    while (!all_done() && wl_display_dispatch(display) != -1) {
    }
    for (Source* s : taken)
        if (!s->done || s->failed) {
            std::fprintf(stderr, "failed to copy %s\n", s->name.empty() ? "the window" : s->name.c_str());
            return 1;
        }

    if (!toplevel_id.empty()) {
        // The window as it is drawn: its buffer is the picture.
        window.mode_width = window.buffer_width;
        window.mode_height = window.buffer_height;
        window.logical = {0, 0, window.buffer_width, window.buffer_height};
        transformed_size(window.transform, window.logical.width, window.logical.height);
        geometry = window.logical;
    } else if (!geometry) {
        std::vector<Box> boxes;
        for (Source* s : taken)
            boxes.push_back(s->logical);
        geometry = extents(boxes);
    }

    pixman_image_t* image = render(taken, *geometry, scale);
    if (!image)
        return 1;
    FILE* f = path == "-" ? stdout : std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "Failed to open file '%s' for writing: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    bool ok = false;
    switch (type) {
    case FileType::Png: ok = write_png(image, f, level); break;
    case FileType::Ppm: ok = write_ppm(image, f); break;
    case FileType::Jpeg: ok = write_jpeg(image, f, quality); break;
    }
    if (f != stdout)
        std::fclose(f);
    pixman_image_unref(image);
    if (!ok) {
        std::fprintf(stderr, "failed to write the picture\n");
        return 1;
    }
    // The buffers' proxies go before the connection does.
    window.buffer.reset();
    st.outputs.clear();
    wl_display_disconnect(display);
    return 0;
}
