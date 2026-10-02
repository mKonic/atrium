// Outputs and their commits, after wlroots' types/output (MIT): the same
// checks, the same blank buffer on a modeset, the same swapchain choice.
#include "render/pass.hpp"
#include "backend/output.hpp"

#include "backend/allocator.hpp"
#include "backend/backend.hpp"

#include "render/renderer.hpp"
#include "wlr.hpp"

#include <drm_fourcc.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

namespace atrium::backend {

// ---- OutputState --------------------------------------------------------------------

OutputState::OutputState() {
    pixman_region32_init(&damage);
}

OutputState::~OutputState() {
    pixman_region32_fini(&damage);
    clear_buffer();
    if (wait_timeline)
        wlr_drm_syncobj_timeline_unref(wait_timeline);
    if (signal_timeline)
        wlr_drm_syncobj_timeline_unref(signal_timeline);
    wlr_color_transform_unref(color_transform);
}

OutputState::OutputState(const OutputState& o) : OutputState() {
    *this = o;
}

OutputState& OutputState::operator=(const OutputState& o) {
    if (this == &o)
        return *this;
    committed = o.committed;
    pixman_region32_copy(&damage, &o.damage);
    enabled = o.enabled;
    scale = o.scale;
    transform = o.transform;
    adaptive_sync_enabled = o.adaptive_sync_enabled;
    render_format = o.render_format;
    subpixel = o.subpixel;
    clear_buffer();
    buffer = o.buffer ? buffer_lock(o.buffer) : nullptr;
    buffer_src_box = o.buffer_src_box;
    buffer_dst_box = o.buffer_dst_box;
    tearing_page_flip = o.tearing_page_flip;
    allow_reconfiguration = o.allow_reconfiguration;
    mode_type = o.mode_type;
    mode = o.mode;
    custom_mode = o.custom_mode;
    set_wait_timeline(o.wait_timeline, o.wait_point);
    set_signal_timeline(o.signal_timeline, o.signal_point);
    committed = o.committed;  // the setters marked them
    wlr_color_transform* ct = o.color_transform ? wlr_color_transform_ref(o.color_transform) : nullptr;
    wlr_color_transform_unref(color_transform);
    color_transform = ct;
    image_description = o.image_description;
    return *this;
}

void OutputState::clear_buffer() {
    if (buffer)
        buffer_unlock(buffer);
    buffer = nullptr;
}

void OutputState::set_enabled(bool on) {
    committed |= Enabled;
    allow_reconfiguration = true;  // as wlroots: these may need a modeset
    enabled = on;
}

void OutputState::set_mode(const Mode* m) {
    committed |= ModeField;
    allow_reconfiguration = true;  // as wlroots: these may need a modeset
    mode_type = ModeType::Fixed;
    mode = m;
}

void OutputState::set_custom_mode(int32_t w, int32_t h, int32_t refresh) {
    committed |= ModeField;
    allow_reconfiguration = true;  // as wlroots: these may need a modeset
    mode_type = ModeType::Custom;
    mode = nullptr;
    custom_mode = {w, h, refresh};
}

void OutputState::set_scale(float s) {
    committed |= Scale;
    scale = s;
}

void OutputState::set_transform(wl_output_transform t) {
    committed |= Transform;
    transform = t;
}

void OutputState::set_adaptive_sync_enabled(bool on) {
    committed |= AdaptiveSyncEnabled;
    adaptive_sync_enabled = on;
}

void OutputState::set_render_format(uint32_t format) {
    committed |= RenderFormat;
    render_format = format;
}

void OutputState::set_subpixel(wl_output_subpixel s) {
    committed |= Subpixel;
    subpixel = s;
}

void OutputState::set_buffer(atrium::Buffer* b) {
    committed |= Buffer;
    atrium::Buffer* locked = b ? buffer_lock(b) : nullptr;
    clear_buffer();
    buffer = locked;
}

void OutputState::set_damage(const pixman_region32_t* d) {
    committed |= Damage;
    pixman_region32_copy(&damage, d);
}

void OutputState::set_wait_timeline(wlr_drm_syncobj_timeline* t, uint64_t point) {
    committed |= WaitTimeline;
    wlr_drm_syncobj_timeline* ref = t ? wlr_drm_syncobj_timeline_ref(t) : nullptr;
    if (wait_timeline)
        wlr_drm_syncobj_timeline_unref(wait_timeline);
    wait_timeline = ref;
    wait_point = point;
}

void OutputState::set_signal_timeline(wlr_drm_syncobj_timeline* t, uint64_t point) {
    committed |= SignalTimeline;
    wlr_drm_syncobj_timeline* ref = t ? wlr_drm_syncobj_timeline_ref(t) : nullptr;
    if (signal_timeline)
        wlr_drm_syncobj_timeline_unref(signal_timeline);
    signal_timeline = ref;
    signal_point = point;
}

void OutputState::set_color_transform(wlr_color_transform* t) {
    committed |= ColorTransform;
    wlr_color_transform* ref = t ? wlr_color_transform_ref(t) : nullptr;
    wlr_color_transform_unref(color_transform);
    color_transform = ref;
}

void OutputState::set_image_description(const ImageDescription* d) {
    committed |= ImageDescriptionField;
    if (d)
        image_description = *d;
    else
        image_description.reset();
}

// ---- Output ---------------------------------------------------------------------------

Output::Output(Backend& b) : backend(b) {}

Output::~Output() {
    if (idle_frame_)
        wl_event_source_remove(idle_frame_);
}

void Output::pending_resolution(const OutputState& state, int* w, int* h) const {
    if (!(state.committed & OutputState::ModeField)) {
        *w = width;
        *h = height;
    } else if (state.mode_type == OutputState::ModeType::Fixed) {
        *w = state.mode ? state.mode->width : 0;
        *h = state.mode ? state.mode->height : 0;
    } else {
        *w = state.custom_mode.width;
        *h = state.custom_mode.height;
    }
}

uint32_t Output::unchanged(const OutputState& s) const {
    uint32_t f = 0;
    if (s.committed & OutputState::ModeField) {
        const bool same = s.mode_type == OutputState::ModeType::Fixed
                              ? current_mode == s.mode
                              : width == s.custom_mode.width && height == s.custom_mode.height &&
                                    refresh == s.custom_mode.refresh;
        if (same)
            f |= OutputState::ModeField;
    }
    if ((s.committed & OutputState::Enabled) && enabled == s.enabled)
        f |= OutputState::Enabled;
    if ((s.committed & OutputState::Scale) && scale == s.scale)
        f |= OutputState::Scale;
    if ((s.committed & OutputState::Transform) && transform == s.transform)
        f |= OutputState::Transform;
    if ((s.committed & OutputState::AdaptiveSyncEnabled) &&
        (adaptive_sync_status != AdaptiveSync::Disabled) == s.adaptive_sync_enabled)
        f |= OutputState::AdaptiveSyncEnabled;
    if ((s.committed & OutputState::RenderFormat) && render_format == s.render_format)
        f |= OutputState::RenderFormat;
    if ((s.committed & OutputState::Subpixel) && subpixel == s.subpixel)
        f |= OutputState::Subpixel;
    return f;
}

namespace {

FBox src_box_of(const OutputState& s) {
    FBox b = s.buffer_src_box;
    if (b.width == 0 && b.height == 0)
        b = {0, 0, double(s.buffer->width), double(s.buffer->height)};
    return b;
}

} // namespace

bool Output::basic_test(const OutputState& s) const {
    const bool on = (s.committed & OutputState::Enabled) ? s.enabled : enabled;
    if (s.committed & OutputState::Buffer) {
        const FBox src = src_box_of(s);
        if (src.x < 0 || src.y < 0 || src.x + src.width > s.buffer->width || src.y + src.height > s.buffer->height ||
            src.width <= 0 || src.height <= 0) {
            alog(Log::Error, "%s: buffer source box outside the buffer", name.c_str());
            return false;
        }
        int w, h;
        pending_resolution(s, &w, &h);
        Box dst = s.buffer_dst_box;
        if (dst.width == 0 && dst.height == 0) {
            dst.width = s.buffer->width;
            dst.height = s.buffer->height;
        }
        Box screen{0, 0, w, h};
        if (!box_intersection(&screen, &screen, &dst)) {
            alog(Log::Error, "%s: buffer entirely off-screen", name.c_str());
            return false;
        }
    } else if (s.tearing_page_flip ||
               (s.committed & (OutputState::WaitTimeline | OutputState::SignalTimeline))) {
        return false;  // they go with a buffer
    }
    if (s.committed & OutputState::RenderFormat) {
        std::vector<uint64_t> mods;
        if (!pick_format(s.render_format, &mods))
            return false;
    }
    if (on && (s.committed & (OutputState::Enabled | OutputState::ModeField))) {
        int w, h;
        pending_resolution(s, &w, &h);
        if (w == 0 || h == 0)
            return false;
    }
    constexpr uint32_t kNeedsEnabled = OutputState::Buffer | OutputState::ModeField |
                                       OutputState::AdaptiveSyncEnabled | OutputState::RenderFormat |
                                       OutputState::Subpixel | OutputState::ColorTransform |
                                       OutputState::ImageDescriptionField;
    if (!on && (s.committed & kNeedsEnabled))
        return false;
    if ((s.committed & OutputState::ImageDescriptionField) && s.image_description) {
        if (!(supported_primaries & s.image_description->primaries) ||
            !(supported_transfer_functions & s.image_description->transfer_function))
            return false;
    }
    return true;
}

bool Output::pick_format(uint32_t fmt, std::vector<uint64_t>* modifiers) const {
    if (!renderer || !allocator)
        return false;
    const FormatSet* render = renderer->egl().render_formats();
    const DrmFormat* rf = render ? render->get(fmt) : nullptr;
    if (!rf)
        return false;
    const FormatSet* display = primary_formats(BUFFER_CAP_DMABUF);
    const DrmFormat* df = display ? display->get(fmt) : nullptr;
    if (display && !df)
        return false;
    modifiers->clear();
    for (uint64_t m : rf->modifiers)
        if (!df || df->has(m))
            modifiers->push_back(m);
    return !modifiers->empty();
}

std::unique_ptr<Swapchain> Output::create_swapchain(int w, int h, uint32_t format, bool allow_modifiers) {
    std::vector<uint64_t> mods;
    if (!pick_format(format, &mods)) {
        alog(Log::Error, "%s: no buffer format 0x%08x both the renderer and screen take", name.c_str(), format);
        return nullptr;
    }
    if (!allow_modifiers && !(mods.size() == 1 && mods[0] == DRM_FORMAT_MOD_LINEAR)) {
        if (std::ranges::find(mods, DRM_FORMAT_MOD_INVALID) == mods.end())
            return nullptr;
        mods = {DRM_FORMAT_MOD_INVALID};
    }
    return std::make_unique<Swapchain>(*allocator, w, h, format, std::move(mods));
}

bool Output::configure_primary_swapchain(const OutputState* state, std::unique_ptr<Swapchain>& out) {
    const OutputState empty;
    if (!state)
        state = &empty;
    int w, h;
    pending_resolution(*state, &w, &h);
    const uint32_t format = (state->committed & OutputState::RenderFormat) ? state->render_format : render_format;
    if (out && out->width == w && out->height == h && out->format == format)
        return true;

    auto passes = [&](Swapchain& sc) {
        Buffer* b = sc.acquire();
        if (!b)
            return false;
        OutputState copy = *state;
        copy.set_buffer(b);
        buffer_unlock(b);
        return test_state(copy);
    };
    std::unique_ptr<Swapchain> sc = create_swapchain(w, h, format, true);
    if (!sc || !passes(*sc)) {
        // Some screens only take buffers laid out the implicit way.
        sc = create_swapchain(w, h, format, false);
        if (!sc || !passes(*sc)) {
            alog(Log::Error, "%s: no swapchain the screen takes", name.c_str());
            return false;
        }
    }
    out = std::move(sc);
    return true;
}

bool Output::ensure_buffer(OutputState& s, bool* added) {
    *added = false;
    if ((s.committed & OutputState::Buffer) || !renderer)
        return true;
    const bool on = (s.committed & OutputState::Enabled) ? s.enabled : enabled;
    const bool needs = ((s.committed & OutputState::Enabled) && s.enabled) ||
                       (s.committed & (OutputState::ModeField | OutputState::RenderFormat)) ||
                       (s.allow_reconfiguration && commit_seq == 0 && on);
    if (!needs)
        return true;
    if (!configure_primary_swapchain(&s, swapchain))
        return false;
    Buffer* b = swapchain->acquire();
    if (!b)
        return false;
    render::RenderPass* pass = renderer->begin_buffer_pass(b, nullptr);
    if (!pass) {
        buffer_unlock(b);
        return false;
    }
    render::RectOptions rect{};
    rect.box = {0, 0, b->width, b->height};
    rect.blend_mode = render::BLEND_MODE_NONE;
    pass->add_rect(&rect);
    if (!pass->submit()) {
        buffer_unlock(b);
        return false;
    }
    s.set_buffer(b);
    buffer_unlock(b);
    *added = true;
    return true;
}

bool Output::prepare_commit(OutputState& s) {
    s.committed &= ~unchanged(s);
    if (!basic_test(s)) {
        alog(Log::Error, "%s: the commit failed the basic checks", name.c_str());
        return false;
    }
    bool added;
    return ensure_buffer(s, &added);
}

void Output::finish_commit(const OutputState& s) {
    apply(s);
    events.commit.emit(s);
}

bool Output::test_state(const OutputState& state) {
    OutputState copy = state;
    copy.committed &= ~unchanged(state);
    if (!basic_test(copy))
        return false;
    bool added;
    if (!ensure_buffer(copy, &added))
        return false;
    return test(copy);
}

bool Output::commit_state(const OutputState& state) {
    OutputState pending = state;
    if (!prepare_commit(pending) || !commit(pending))
        return false;
    finish_commit(pending);
    return true;
}

void Output::apply(const OutputState& s) {
    if (s.committed & OutputState::RenderFormat)
        render_format = s.render_format;
    if (s.committed & OutputState::Subpixel)
        subpixel = s.subpixel;
    if (s.committed & OutputState::Enabled)
        enabled = s.enabled;
    if (s.committed & OutputState::Scale)
        scale = s.scale;
    if (s.committed & OutputState::Transform)
        transform = s.transform;
    if (s.committed & OutputState::AdaptiveSyncEnabled)
        adaptive_sync_status = s.adaptive_sync_enabled ? AdaptiveSync::Enabled : AdaptiveSync::Disabled;
    if (s.committed & OutputState::ImageDescriptionField)
        image_description = s.image_description;
    if ((s.committed & OutputState::Enabled) && !s.enabled)
        swapchain.reset();
    if (s.committed & OutputState::ModeField) {
        int w = 0, h = 0, r = 0;
        if (s.mode_type == OutputState::ModeType::Fixed) {
            current_mode = s.mode;
            if (s.mode) {
                w = s.mode->width;
                h = s.mode->height;
                r = s.mode->refresh;
            }
        } else {
            current_mode = nullptr;
            w = s.custom_mode.width;
            h = s.custom_mode.height;
            r = s.custom_mode.refresh;
        }
        width = w;
        height = h;
        refresh = r;
        if (swapchain && (swapchain->width != w || swapchain->height != h))
            swapchain.reset();
    }
    ++commit_seq;
    if (s.committed & OutputState::Buffer) {
        frame_pending = true;
        needs_frame = false;
    }
}

void Output::schedule_frame_impl() {
    if (frame_pending || idle_frame_)
        return;
    // Idle: a buffer may be committed right after this call.
    idle_frame_ = wl_event_loop_add_idle(
        backend.loop(),
        [](void* data) {
            auto* o = static_cast<Output*>(data);
            o->idle_frame_ = nullptr;
            if (!o->frame_pending)
                o->send_frame();
        },
        this);
}

void Output::schedule_frame() {
    // A frame even without a new buffer: clients waiting on frame callbacks.
    update_needs_frame();
    schedule_frame_impl();
}

void Output::update_needs_frame() {
    if (needs_frame)
        return;
    needs_frame = true;
    events.needs_frame.emit();
}

void Output::send_frame() {
    frame_pending = false;
    if (enabled)
        events.frame.emit();
}

void Output::send_present(Present p) {
    if (p.presented && p.when.tv_sec == 0 && p.when.tv_nsec == 0)
        clock_gettime(CLOCK_MONOTONIC, &p.when);
    events.present.emit(p);
}

const Mode* Output::preferred_mode() const {
    for (const Mode& m : modes)
        if (m.preferred)
            return &m;
    return modes.empty() ? nullptr : &modes.front();
}

void Output::transformed_resolution(int* w, int* h) const {
    if (transform % 2 == 0) {
        *w = width;
        *h = height;
    } else {
        *w = height;
        *h = width;
    }
}

void Output::effective_resolution(int* w, int* h) const {
    transformed_resolution(w, h);
    *w = int(std::round(*w / scale));
    *h = int(std::round(*h / scale));
}

const FormatSet* Output::primary_formats(uint32_t) const {
    return nullptr;
}

bool Output::init_render(Allocator* a, render::Renderer* r) {
    swapchain.reset();
    allocator = a;
    renderer = r;
    return a && r;
}

// ---- Backend --------------------------------------------------------------------------

bool Backend::commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) {
    // One by one: no backend of ours can do them atomically yet.
    for (const auto& [o, st] : states)
        if (!(test_only ? o->test_state(st) : o->commit_state(st)))
            return false;
    return true;
}

Multi::~Multi() {
    connections_.clear();
    backends_.clear();
    events.destroy.emit();
}

void Multi::add(std::unique_ptr<Backend> b) {
    Backend* raw = b.get();
    connections_.push_back(raw->events.new_output.connect([this](Output* o) { events.new_output.emit(o); }));
    connections_.push_back(raw->events.new_input.connect([this](wlr_input_device* d) { events.new_input.emit(d); }));
    connections_.push_back(raw->events.host_motion.connect(
        [this](Output* o, uint32_t t, double fx, double fy) { events.host_motion.emit(o, t, fx, fy); }));
    connections_.push_back(raw->events.host_button.connect(
        [this](uint32_t t, uint32_t b, bool p) { events.host_button.emit(t, b, p); }));
    connections_.push_back(raw->events.host_axis.connect([this](const HostAxis& a) { events.host_axis.emit(a); }));
    connections_.push_back(raw->events.host_frame.connect([this] { events.host_frame.emit(); }));
    connections_.push_back(
        raw->events.host_key.connect([this](uint32_t t, uint32_t k, bool p) { events.host_key.emit(t, k, p); }));
    connections_.push_back(raw->events.gone.connect([this] { events.gone.emit(); }));
    backends_.push_back(std::move(b));
    if (started_)
        raw->start();
}

void Multi::remove(Backend* b) {
    auto it = std::ranges::find_if(backends_, [b](const auto& x) { return x.get() == b; });
    if (it == backends_.end())
        return;
    std::unique_ptr<Backend> gone = std::move(*it);
    backends_.erase(it);
    gone.reset();
}

bool Multi::start() {
    started_ = true;
    for (auto& b : backends_)
        if (!b->start())
            return false;
    return true;
}

int Multi::drm_fd() const {
    for (const auto& b : backends_)
        if (b->drm_fd() >= 0)
            return b->drm_fd();
    return -1;
}

uint32_t Multi::buffer_caps() const {
    uint32_t caps = 0;
    bool first = true;
    for (const auto& b : backends_) {
        caps = first ? b->buffer_caps() : (caps & b->buffer_caps());
        first = false;
    }
    return caps;
}

bool Multi::commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) {
    // Each backend gets its own outputs' states.
    for (const auto& b : backends_) {
        std::vector<std::pair<Output*, OutputState>> mine;
        for (const auto& [o, st] : states)
            if (&o->backend == b.get())
                mine.emplace_back(o, st);
        if (!mine.empty() && !b->commit(mine, test_only))
            return false;
    }
    return true;
}

Output* Multi::create_output() {
    for (auto& b : backends_)
        if (Output* o = b->create_output())
            return o;
    return nullptr;
}

bool Multi::is_virtual(const Output* o) const {
    return std::ranges::any_of(backends_, [o](const auto& b) { return b->is_virtual(o); });
}

bool Multi::destroy_output(Output* o) {
    return std::ranges::any_of(backends_, [o](const auto& b) { return b->destroy_output(o); });
}

bool Multi::supports_timelines() const {
    return !backends_.empty() &&
           std::ranges::all_of(backends_, [](const auto& b) { return b->supports_timelines(); });
}

bool Multi::is_drm() const {
    return std::ranges::any_of(backends_, [](const auto& b) { return b->is_drm(); });
}

} // namespace atrium::backend
