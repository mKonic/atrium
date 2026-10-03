#include "screenshot.hpp"

#include "output.hpp"
#include "screenshot_core.hpp"
#include "server.hpp"
#include "view.hpp"
#include "wl/capture.hpp"

#include <cairo.h>
#include <drm_fourcc.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace atrium {

namespace {

// A buffer in memory the copy reads pixels into.
struct MemoryBuffer {
    Buffer base;
    std::vector<uint8_t> data;
    size_t stride = 0;

    static MemoryBuffer* from(Buffer* b) { return reinterpret_cast<MemoryBuffer*>(b); }
    static const BufferImpl impl;
};

const BufferImpl MemoryBuffer::impl = {
    .destroy = [](Buffer* b) {
        buffer_finish(b);
        delete MemoryBuffer::from(b);
    },
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = [](Buffer* b, uint32_t, void** data, uint32_t* format, size_t* stride) {
        MemoryBuffer* m = MemoryBuffer::from(b);
        *data = m->data.data();
        *format = DRM_FORMAT_ARGB8888;
        *stride = m->stride;
        return true;
    },
    .end_data_ptr_access = [](Buffer*) {},
};

Buffer* memory_buffer(int width, int height) {
    auto* m = new MemoryBuffer;
    m->stride = size_t(width) * 4;
    m->data.resize(m->stride * size_t(height));
    buffer_init(&m->base, &MemoryBuffer::impl, width, height);
    return &m->base;
}

// One screen's or window's part of a shot.
struct Part {
    wl::Capture::Target target;
    Box logical;  // where it goes, layout coordinates
    wl_output_transform transform = WL_OUTPUT_TRANSFORM_NORMAL;
    std::vector<uint32_t> pixels;  // upright, once copied
    int width = 0, height = 0;
    int tries = 0;
};

struct Shot {
    Server* server;
    std::vector<Part> parts;
    Box area;
    double scale = 1;
    std::string path;
    std::function<void(const std::string&)> done;
    size_t waiting = 0;
    bool failed = false;
};

void finish(const std::shared_ptr<Shot>& shot) {
    if (shot->failed)
        return;
    // A window at its own size, unless asked otherwise.
    if (shot->scale == 0)
        shot->scale = shot->parts[0].width / double(std::max(1, shot->parts[0].logical.width));
    const int w = std::max(1, int(std::lround(shot->area.width * shot->scale)));
    const int h = std::max(1, int(std::lround(shot->area.height * shot->scale)));
    cairo_surface_t* canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(canvas);
    for (Part& p : shot->parts) {
        if (p.pixels.empty())
            continue;
        cairo_surface_t* image = cairo_image_surface_create_for_data(
            reinterpret_cast<unsigned char*>(p.pixels.data()), CAIRO_FORMAT_ARGB32, p.width, p.height, p.width * 4);
        cairo_save(cr);
        cairo_translate(cr, (p.logical.x - shot->area.x) * shot->scale, (p.logical.y - shot->area.y) * shot->scale);
        cairo_scale(cr, p.logical.width * shot->scale / p.width, p.logical.height * shot->scale / p.height);
        cairo_set_source_surface(cr, image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(image);
    }
    cairo_destroy(cr);
    const cairo_status_t status = cairo_surface_write_to_png(canvas, shot->path.c_str());
    cairo_surface_destroy(canvas);
    shot->done(status == CAIRO_STATUS_SUCCESS ? "" : std::string("can't write it: ") + cairo_status_to_string(status));
}

void fail(const std::shared_ptr<Shot>& shot, const std::string& why) {
    if (std::exchange(shot->failed, true))
        return;
    shot->done(why);
}

void copy(const std::shared_ptr<Shot>& shot, size_t index) {
    Part& part = shot->parts[index];
    wl::Capture& cap = *shot->server->wl->capture;
    const auto c = cap.constraints ? cap.constraints(part.target) : std::nullopt;
    if (!c)
        return fail(shot, "nothing to capture there");
    Buffer* buffer = memory_buffer(c->width, c->height);
    wl::Capture::Copy request{
        .target = part.target,
        .buffer = buffer,
        .wait_for_damage = false,
        .done = [shot, index, buffer](const wl::Capture::Result& r) {
            Part& p = shot->parts[index];
            if (r.ok) {
                const MemoryBuffer* m = MemoryBuffer::from(buffer);
                p.pixels = screenshot_upright(m->data.data(), buffer->width, buffer->height, m->stride, p.transform,
                                              &p.width, &p.height);
            }
            buffer_drop(buffer);
            if (!r.ok && r.fail_reason == 1 && ++p.tries < 3)
                return copy(shot, index);  // a window changed size: again
            if (!r.ok)
                return fail(shot, "the capture failed");
            if (--shot->waiting == 0)
                finish(shot);
        },
    };
    cap.copy.emit(request);
}

} // namespace

void take_screenshot(Server& server, const ScreenshotRequest& req, std::function<void(const std::string&)> done) {
    auto shot = std::make_shared<Shot>();
    shot->server = &server;
    shot->path = req.path;
    shot->done = std::move(done);
    if (req.path.empty() || req.path[0] != '/')
        return shot->done("needs an absolute \"path\"");

    if (req.window || !req.identifier.empty()) {
        View* view = nullptr;
        for (View* v : server.views)
            if ((req.window && v->id == *req.window) ||
                (!req.identifier.empty() && v->toplevel_identifier() == req.identifier))
                view = v;
        if (!view || !view->toplevel_handle())
            return shot->done("no such window");
        const int top = view->top();
        shot->area = {view->geom.x, view->geom.y + top, view->geom.width, view->geom.height - top};
        shot->parts.push_back({.target = {.toplevel = view->toplevel_handle()}, .logical = shot->area});
        shot->scale = req.scale.value_or(0);  // 0: the window's own (finish)
    } else {
        std::vector<Box> boxes;
        std::vector<Output*> shown;
        for (Output* o : server.outputs)
            if (o->enabled() && o->global && (req.output.empty() || o->screen->name == req.output)) {
                boxes.push_back(o->box);
                shown.push_back(o);
            }
        if (shown.empty())
            return shot->done(req.output.empty() ? "no screens" : "no such screen");
        shot->area = screenshot_area(boxes, req.region);
        if (shot->area.width <= 0 || shot->area.height <= 0)
            return shot->done("nothing shown there");
        double largest = 0;
        for (Output* o : shown) {
            Box part{};
            if (!box_intersection(&part, &o->box, &shot->area))
                continue;
            largest = std::max(largest, double(o->screen->scale));
            shot->parts.push_back({.target = {.output = o->global.get(),
                                              .region = part == o->box ? std::nullopt : std::optional<Box>(part)},
                                   .logical = part,
                                   .transform = o->screen->transform});
        }
        shot->scale = req.scale.value_or(largest);
    }
    if (req.scale && (*req.scale <= 0 || *req.scale > 8))
        return shot->done("\"scale\" goes from above 0 to 8");
    shot->waiting = shot->parts.size();
    for (size_t i = 0; i < shot->parts.size(); ++i)
        copy(shot, i);
}

} // namespace atrium
