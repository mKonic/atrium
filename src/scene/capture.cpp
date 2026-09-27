// A capture source of a subtree: a private output the subtree is drawn on,
// sized to what's in it. Ported from wlroots'
// ext_image_capture_source_v1/scene.c (MIT).

#include "scene/internal.hpp"

#include <climits>
#include <cstdio>

extern "C" {
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/backend/interface.h>
}

namespace atrium::scene {

namespace {

struct Source;

// wlroots' embedded structs, each with a way back to the source.
struct SourceHook {
    wlr_ext_image_capture_source_v1 base;
    Source* self;
};
struct OutputHook {
    wlr_output base;
    Source* self;
};

struct FrameEvent {
    wlr_ext_image_capture_source_v1_frame_event base;
    wlr_buffer* buffer;
    timespec when;
};

struct Source {
    SourceHook source{};
    OutputHook output{};
    wlr_backend backend{};
    Node* node = nullptr;
    SceneOutput* scene_output = nullptr;
    size_t started = 0;
    Listener<> node_destroy, scene_output_destroy, output_frame;

    void render();
    void destroy();
};

size_t g_last_output = 0;

void extents(Node* node, const Walk& w, int* x1, int* y1, int* x2, int* y2) {
    if (node->type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(node);
        Node* child;
        wl_list_for_each(child, &tree->children, link)
            extents(child, w.child(tree, child), x1, y1, x2, y2);
        return;
    }
    if (node->type != Type::Rect && node->type != Type::Buffer)
        return;
    const wlr_box b = box_of(node, w);
    *x1 = std::min(*x1, b.x);
    *y1 = std::min(*y1, b.y);
    *x2 = std::max(*x2, b.x + b.width);
    *y2 = std::max(*y2, b.y + b.height);
}

void Source::render() {
    int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
    extents(node, walk_of(node), &x1, &y1, &x2, &y2);
    if (x2 <= x1 || y2 <= y1)
        return;
    scene_output->set_position(x1, y1);
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, x2 - x1, y2 - y1, 0);
    scene_output->build_state(&state) && wlr_output_commit_state(&output.base, &state);
    wlr_output_state_finish(&state);
}

void Source::destroy() {
    node_destroy.disconnect();
    scene_output_destroy.disconnect();
    output_frame.disconnect();
    wlr_ext_image_capture_source_v1_finish(&source.base);
    if (scene_output)
        scene_output->destroy();
    wlr_output_finish(&output.base);
    wlr_backend_finish(&backend);
    delete this;
}

Source* of(wlr_ext_image_capture_source_v1* s) { return reinterpret_cast<SourceHook*>(s)->self; }
Source* of(wlr_output* o) { return reinterpret_cast<OutputHook*>(o)->self; }

void source_start(wlr_ext_image_capture_source_v1* base, bool) {
    Source* s = of(base);
    if (++s->started > 1)
        return;
    s->render();
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    s->scene_output->send_frame_done(&now);
}

void source_stop(wlr_ext_image_capture_source_v1* base) {
    Source* s = of(base);
    if (--s->started > 0)
        return;
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, false);
    wlr_output_commit_state(&s->output.base, &state);
    wlr_output_state_finish(&state);
}

void source_request_frame(wlr_ext_image_capture_source_v1* base, bool schedule) {
    Source* s = of(base);
    if (s->output.base.frame_pending)
        wlr_output_send_frame(&s->output.base);
    if (schedule)
        wlr_output_update_needs_frame(&s->output.base);
}

void source_copy_frame(wlr_ext_image_capture_source_v1* base, wlr_ext_image_copy_capture_frame_v1* frame,
                       wlr_ext_image_capture_source_v1_frame_event* ev) {
    Source* s = of(base);
    auto* e = reinterpret_cast<FrameEvent*>(ev);
    if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, e->buffer, s->output.base.renderer))
        wlr_ext_image_copy_capture_frame_v1_ready(frame, s->output.base.transform, &e->when);
}

const wlr_ext_image_capture_source_v1_interface kSourceImpl = {
    .start = source_start,
    .stop = source_stop,
    .request_frame = source_request_frame,
    .copy_frame = source_copy_frame,
};

const wlr_backend_impl kBackendImpl = {};

bool output_test(wlr_output* o, const wlr_output_state* st) {
    const uint32_t supported = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
                               WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;
    if (st->committed & ~supported)
        return false;
    if (st->committed & WLR_OUTPUT_STATE_BUFFER) {
        int w = o->width, h = o->height;
        if (st->committed & WLR_OUTPUT_STATE_MODE) {
            w = st->custom_mode.width;
            h = st->custom_mode.height;
        }
        if (st->buffer->width != w || st->buffer->height != h)
            return false;
        const wlr_fbox& src = st->buffer_src_box;
        if (!(src.width == 0 && src.height == 0) &&
            (src.x != 0 || src.y != 0 || src.width != st->buffer->width || src.height != st->buffer->height))
            return false;
    }
    return true;
}

bool output_commit(wlr_output* o, const wlr_output_state* st) {
    Source* s = of(o);
    if ((st->committed & WLR_OUTPUT_STATE_ENABLED) && !st->enabled)
        return true;
    if ((st->committed & WLR_OUTPUT_STATE_MODE) &&
        wlr_output_configure_primary_swapchain(o, st, &o->swapchain))
        wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&s->source.base, o->swapchain, o->renderer);
    if (!(st->committed & WLR_OUTPUT_STATE_BUFFER))
        return false;
    wlr_buffer* buffer = st->buffer;
    pixman_region32_t full;
    pixman_region32_init_rect(&full, 0, 0, unsigned(buffer->width), unsigned(buffer->height));
    FrameEvent ev{};
    ev.base.damage = (st->committed & WLR_OUTPUT_STATE_DAMAGE) ? &st->damage : &full;
    ev.buffer = buffer;
    clock_gettime(CLOCK_MONOTONIC, &ev.when);
    wl_signal_emit_mutable(&s->source.base.events.frame, &ev.base);
    pixman_region32_fini(&full);
    return true;
}

const wlr_output_impl kOutputImpl = {
    .test = output_test,
    .commit = output_commit,
};

} // namespace

wlr_ext_image_capture_source_v1* capture_source_create(Node* node, wl_event_loop* loop, wlr_allocator* allocator,
                                                       wlr_renderer* renderer) {
    auto* s = new Source();
    s->node = node;
    s->source.self = s;
    s->output.self = s;
    wlr_ext_image_capture_source_v1_init(&s->source.base, &kSourceImpl);
    wlr_backend_init(&s->backend, &kBackendImpl);
    s->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;
    wlr_output_init(&s->output.base, &s->backend, &kOutputImpl, loop, nullptr);
    char name[64];
    std::snprintf(name, sizeof(name), "CAPTURE-%zu", ++g_last_output);
    wlr_output_set_name(&s->output.base, name);
    wlr_output_init_render(&s->output.base, allocator, renderer);
    s->scene_output = SceneOutput::create(node->root(), &s->output.base);

    s->node_destroy.connect(&node->events.destroy, [s](void*) { s->destroy(); });
    s->scene_output_destroy.connect(&s->scene_output->events.destroy, [s](void*) {
        s->scene_output = nullptr;
        s->scene_output_destroy.disconnect();
    });
    s->output_frame.connect(&s->output.base.events.frame, [s](void*) {
        if (!s->scene_output || !s->scene_output->needs_frame())
            return;
        // Frames go out only with damage.
        if (pixman_region32_not_empty(&s->scene_output->pending_commit_damage))
            s->render();
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        s->scene_output->send_frame_done(&now);
    });
    return &s->source.base;
}

} // namespace atrium::scene
