#include "cairo_buffer.hpp"

#include <drm_fourcc.h>

namespace atrium {

namespace {

struct CairoBuffer {
    wlr_buffer base;
    cairo_surface_t* surface;
};

void cairo_buffer_destroy(wlr_buffer* b) {
    auto* cb = reinterpret_cast<CairoBuffer*>(b);
    cairo_surface_destroy(cb->surface);
    delete cb;
}

bool cairo_buffer_begin(wlr_buffer* b, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    auto* cb = reinterpret_cast<CairoBuffer*>(b);
    *data = cairo_image_surface_get_data(cb->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = size_t(cairo_image_surface_get_stride(cb->surface));
    return true;
}

void cairo_buffer_end(wlr_buffer*) {}

const wlr_buffer_impl kCairoBufferImpl = {
    .destroy = cairo_buffer_destroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = cairo_buffer_begin,
    .end_data_ptr_access = cairo_buffer_end,
};

} // namespace

wlr_buffer* set_cairo_buffer(wlr_scene_buffer* node, cairo_surface_t* surface, int width, int height) {
    auto* cb = new CairoBuffer{};
    cb->surface = surface;
    wlr_buffer_init(&cb->base, &kCairoBufferImpl, cairo_image_surface_get_width(surface),
                    cairo_image_surface_get_height(surface));
    wlr_scene_buffer_set_buffer(node, &cb->base);
    wlr_buffer* held = wlr_buffer_lock(&cb->base);
    wlr_buffer_drop(&cb->base);
    wlr_scene_buffer_set_dest_size(node, width, height);
    return held;
}

} // namespace atrium
