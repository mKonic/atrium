// A capture source of a subtree: a private output the subtree is drawn on,
// sized to what's in it. After wlroots' ext_image_capture_source_v1/scene.c
// (MIT).

#include "scene/internal.hpp"

#include "backend/backend.hpp"

#include <climits>
#include <cstdio>
#include <unordered_map>

namespace atrium::scene {

namespace {

struct Source;

// Takes whatever buffer it is given; nothing is shown anywhere.
class CaptureBackend final : public backend::Backend {
public:
    explicit CaptureBackend(wl_event_loop* loop) : Backend(loop) {}
    bool start() override { return true; }
    uint32_t buffer_caps() const override { return BUFFER_CAP_DMABUF | BUFFER_CAP_SHM; }
};

class CaptureOutput final : public backend::Output {
public:
    CaptureOutput(backend::Backend& b, Source* s) : Output(b), self(s) {}
    Source* const self;

protected:
    bool test(const backend::OutputState& st) override;
    bool commit(const backend::OutputState& st) override;
};

struct Source {
    explicit Source(wl_event_loop* loop) : backend(loop), output(backend, this) {}
    CaptureSource* pub = nullptr;
    CaptureBackend backend;
    CaptureOutput output;
    Node* node = nullptr;
    SceneOutput* scene_output = nullptr;
    size_t started = 0;
    Listener<> node_destroy, scene_output_destroy;
    wl::Connection output_frame;

    void render();
    void destroy();
};

size_t g_last_output = 0;

bool CaptureOutput::test(const backend::OutputState& st) {
    constexpr uint32_t kSupported = backend::OutputState::Buffer | backend::OutputState::Damage |
                                    backend::OutputState::Enabled | backend::OutputState::ModeField |
                                    backend::OutputState::RenderFormat;
    if (st.committed & ~kSupported)
        return false;
    if (st.committed & backend::OutputState::Buffer) {
        int w, h;
        pending_resolution(st, &w, &h);
        if (st.buffer->width != w || st.buffer->height != h)
            return false;
        const FBox& src = st.buffer_src_box;
        if (!(src.width == 0 && src.height == 0) &&
            (src.x != 0 || src.y != 0 || src.width != st.buffer->width || src.height != st.buffer->height))
            return false;
    }
    return true;
}

bool CaptureOutput::commit(const backend::OutputState& st) {
    if ((st.committed & backend::OutputState::Enabled) && !st.enabled)
        return true;
    if (!(st.committed & backend::OutputState::Buffer))
        return !(st.committed & backend::OutputState::ModeField) || configure_primary_swapchain(&st, swapchain);
    atrium::Buffer* buffer = st.buffer;
    pixman_region32_t full;
    pixman_region32_init_rect(&full, 0, 0, unsigned(buffer->width), unsigned(buffer->height));
    timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);
    if (self->pub->on_frame)
        self->pub->on_frame(buffer, (st.committed & backend::OutputState::Damage) ? &st.damage : &full, when);
    pixman_region32_fini(&full);
    return true;
}

void extents(Node* node, const Walk& w, int* x1, int* y1, int* x2, int* y2) {
    if (node->type == Type::Tree) {
        Tree* tree = static_cast<Tree*>(node);
        for (Node* child : each_child(tree))
            extents(child, w.child(tree, child), x1, y1, x2, y2);
        return;
    }
    if (node->type != Type::Rect && node->type != Type::Buffer)
        return;
    const Box b = box_of(node, w);
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
    backend::OutputState state;
    state.set_enabled(true);
    state.set_custom_mode(x2 - x1, y2 - y1, 0);
    scene_output->build_state(&state) && output.commit_state(state);
}

std::unordered_map<const CaptureSource*, Source*>& sources() {
    static std::unordered_map<const CaptureSource*, Source*> m;
    return m;
}

void Source::destroy() {
    node_destroy.disconnect();
    scene_output_destroy.disconnect();
    output_frame.disconnect();
    if (scene_output)
        scene_output->destroy();
    output.events.destroy.emit();
    sources().erase(pub);
    delete pub;
    delete this;
}

} // namespace

CaptureSource* CaptureSource::create(Node* node, wl_event_loop* loop, backend::Allocator* allocator,
                                     render::Renderer* renderer) {
    auto* s = new Source(loop);
    s->pub = new CaptureSource();
    sources()[s->pub] = s;
    s->node = node;
    s->output.name = "CAPTURE-" + std::to_string(++g_last_output);
    s->output.init_render(allocator, renderer);
    s->scene_output = SceneOutput::create(node->root(), &s->output);

    s->node_destroy.connect(&node->events.destroy, [s](void*) { s->destroy(); });
    s->scene_output_destroy.connect(&s->scene_output->events.destroy, [s](void*) {
        s->scene_output = nullptr;
        s->scene_output_destroy.disconnect();
    });
    s->output_frame = s->output.events.frame.connect([s] {
        if (!s->scene_output || !s->scene_output->needs_frame())
            return;
        // Frames go out only with damage.
        if (pixman_region32_not_empty(&s->scene_output->pending_commit_damage))
            s->render();
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        s->scene_output->send_frame_done(&now);
    });
    return s->pub;
}

void CaptureSource::destroy() {
    sources().at(this)->destroy();
}

void CaptureSource::start() {
    Source* s = sources().at(this);
    if (++s->started > 1)
        return;
    s->render();
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s->scene_output)
        s->scene_output->send_frame_done(&now);
}

void CaptureSource::stop() {
    Source* s = sources().at(this);
    if (s->started == 0 || --s->started > 0)
        return;
    backend::OutputState state;
    state.set_enabled(false);
    s->output.commit_state(state);
}

void CaptureSource::request_frame(bool force) {
    Source* s = sources().at(this);
    if (force && s->scene_output) {
        // Drawn whole now, changed or not: a still window's first frame.
        s->scene_output->damage_whole();
        s->render();
        return;
    }
    if (s->output.frame_pending)
        s->output.send_frame();
    s->output.update_needs_frame();
}

int CaptureSource::width() const {
    return sources().at(this)->output.width;
}

int CaptureSource::height() const {
    return sources().at(this)->output.height;
}

} // namespace atrium::scene
