#include "cairo_buffer.hpp"

#include <drm_fourcc.h>

namespace atrium {

namespace {

struct CairoBuffer {
    Buffer base;
    cairo_surface_t* surface;
};

void cairo_buffer_destroy(Buffer* b) {
    auto* cb = reinterpret_cast<CairoBuffer*>(b);
    cairo_surface_destroy(cb->surface);
    delete cb;
}

bool cairo_buffer_begin(Buffer* b, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
    if (flags & BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    auto* cb = reinterpret_cast<CairoBuffer*>(b);
    *data = cairo_image_surface_get_data(cb->surface);
    *format = DRM_FORMAT_ARGB8888;
    *stride = size_t(cairo_image_surface_get_stride(cb->surface));
    return true;
}

void cairo_buffer_end(Buffer*) {}

const BufferImpl kCairoBufferImpl = {
    .destroy = cairo_buffer_destroy,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = cairo_buffer_begin,
    .end_data_ptr_access = cairo_buffer_end,
};

} // namespace

Buffer* set_cairo_buffer(scene::Buffer* node, cairo_surface_t* surface, int width, int height) {
    auto* cb = new CairoBuffer{};
    cb->surface = surface;
    buffer_init(&cb->base, &kCairoBufferImpl, cairo_image_surface_get_width(surface),
                    cairo_image_surface_get_height(surface));
    node->set_buffer(&cb->base);
    Buffer* held = buffer_lock(&cb->base);
    buffer_drop(&cb->base);
    node->set_dest_size(width, height);
    return held;
}

} // namespace atrium
