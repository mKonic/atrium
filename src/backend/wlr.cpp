#include "backend/wlr.hpp"

extern "C" {
#include <wlr/render/drm_syncobj.h>
}

#include <algorithm>
#include <unistd.h>

namespace atrium::backend {

namespace {

ImageDescription from_wlr(const wlr_output_image_description& d) {
    ImageDescription out;
    out.primaries = d.primaries;
    out.transfer_function = d.transfer_function;
    out.mastering_display_primaries = d.mastering_display_primaries;
    out.mastering_luminance = {d.mastering_luminance.min, d.mastering_luminance.max};
    out.max_cll = d.max_cll;
    out.max_fall = d.max_fall;
    return out;
}

wlr_output_image_description to_wlr(const ImageDescription& d) {
    wlr_output_image_description out{};
    out.primaries = d.primaries;
    out.transfer_function = d.transfer_function;
    out.mastering_display_primaries = d.mastering_display_primaries;
    out.mastering_luminance.min = d.mastering_luminance.min;
    out.mastering_luminance.max = d.mastering_luminance.max;
    out.max_cll = d.max_cll;
    out.max_fall = d.max_fall;
    return out;
}

// The same syncobj as a wlroots timeline (one reference, the caller's).
wlr_drm_syncobj_timeline* to_wlr(Timeline* t) {
    const int fd = timeline_export(t);
    if (fd < 0)
        return nullptr;
    wlr_drm_syncobj_timeline* out = wlr_drm_syncobj_timeline_import(t->drm_fd, fd);
    close(fd);
    return out;
}

// wlroots' formats copied into `out` (null stays null).
const FormatSet* own(const wlr_drm_format_set* set, FormatSet& out) {
    if (!set)
        return nullptr;
    out.clear();
    for (size_t i = 0; i < set->len; ++i)
        for (size_t k = 0; k < set->formats[i].len; ++k)
            out.add(set->formats[i].format, set->formats[i].modifiers[k]);
    return &out;
}

// atrium's buffers as wlroots' (its DRM backend scans them out): one
// wlr_buffer per buffer, kept on it so wlroots' framebuffer cache holds. It
// locks ours while wlroots holds it.
struct Proxy {
    wlr_buffer base;
    Buffer* ours = nullptr;
    bool holding = false;
    Addon addon{};
    wl_listener release{};
};

const wlr_buffer_impl kProxyImpl = {
    .destroy =
        [](wlr_buffer* b) {
            auto* p = reinterpret_cast<Proxy*>(b);
            wl_list_remove(&p->release.link);
            wlr_buffer_finish(b);
            delete p;
        },
    .get_dmabuf =
        [](wlr_buffer* b, wlr_dmabuf_attributes* out) {
            auto* p = reinterpret_cast<Proxy*>(b);
            DmabufAttributes a;
            if (!p->ours || !buffer_get_dmabuf(p->ours, &a))
                return false;
            *out = {};
            out->width = a.width;
            out->height = a.height;
            out->format = a.format;
            out->modifier = a.modifier;
            out->n_planes = a.n_planes;
            for (int i = 0; i < a.n_planes; ++i) {
                out->offset[i] = a.offset[i];
                out->stride[i] = a.stride[i];
                out->fd[i] = a.fd[i];
            }
            return true;
        },
    .get_shm =
        [](wlr_buffer* b, wlr_shm_attributes* out) {
            auto* p = reinterpret_cast<Proxy*>(b);
            ShmAttributes a;
            if (!p->ours || !buffer_get_shm(p->ours, &a))
                return false;
            *out = {a.fd, a.format, a.width, a.height, a.stride, a.offset};
            return true;
        },
    .begin_data_ptr_access =
        [](wlr_buffer* b, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
            auto* p = reinterpret_cast<Proxy*>(b);
            return p->ours && buffer_begin_data_ptr_access(p->ours, flags, data, format, stride);
        },
    .end_data_ptr_access = [](wlr_buffer* b) { buffer_end_data_ptr_access(reinterpret_cast<Proxy*>(b)->ours); },
};

const AddonInterface kProxyAddon = {
    .name = "atrium_wlr_proxy",
    .destroy =
        [](Addon* a) {
            // Ours is going (so wlroots holds the proxy no longer).
            Proxy* p = wl_container_of(a, p, addon);
            addon_finish(&p->addon);
            p->ours = nullptr;
            wlr_buffer_drop(&p->base);
        },
};

// The wlr_buffer for `b`, ours locked while wlroots holds it.
wlr_buffer* proxy_for(Buffer* b) {
    if (!b)
        return nullptr;
    Proxy* p;
    if (Addon* a = addon_find(&b->addons, &kProxyImpl, &kProxyAddon)) {
        p = wl_container_of(a, p, addon);
    } else {
        p = new Proxy();
        wlr_buffer_init(&p->base, &kProxyImpl, b->width, b->height);
        p->ours = b;
        addon_init(&p->addon, &b->addons, &kProxyImpl, &kProxyAddon);
        p->release.notify = [](wl_listener* l, void*) {
            Proxy* proxy = wl_container_of(l, proxy, release);
            if (proxy->holding) {
                proxy->holding = false;
                buffer_unlock(proxy->ours);  // may destroy ours, and with it the proxy's addon
            }
        };
        wl_signal_add(&p->base.events.release, &p->release);
    }
    if (!p->holding) {
        buffer_lock(b);
        p->holding = true;
    }
    return &p->base;
}

// wlroots didn't keep it (a refused cursor): no release will come.
void let_go_unless_held(wlr_buffer* b) {
    auto* p = reinterpret_cast<Proxy*>(b);
    if (p && p->base.n_locks == 0 && p->holding) {
        p->holding = false;
        buffer_unlock(p->ours);
    }
}

} // namespace

class WlrBackend::WlrOutput final : public Output {
public:
    WlrOutput(WlrBackend& b, wlr_output* o) : Output(b), wlr(o), owner_(b) {
        name = o->name ? o->name : "";
        description = o->description ? o->description : "";
        make = o->make ? o->make : "";
        model = o->model ? o->model : "";
        serial = o->serial ? o->serial : "";
        phys_width = o->phys_width;
        phys_height = o->phys_height;
        non_desktop = o->non_desktop;
        adaptive_sync_supported = o->adaptive_sync_supported;
        supported_primaries = o->supported_primaries;
        supported_transfer_functions = o->supported_transfer_functions;
        if (o->default_primaries)
            default_primaries = *o->default_primaries;
        sync_modes();
        sync_fields();

        frame_.connect(&o->events.frame, [this](void*) { send_frame(); });
        needs_frame_.connect(&o->events.needs_frame, [this](void*) { update_needs_frame(); });
        damage_.connect(&o->events.damage,
                        [this](wlr_output_event_damage* e) { events.damage.emit(e->damage); });
        present_.connect(&o->events.present, [this](wlr_output_event_present* e) {
            Present p;
            p.commit_seq = e->commit_seq + seq_delta_;
            p.presented = e->presented;
            p.when = e->when;
            p.seq = e->seq;
            p.refresh = e->refresh;
            p.flags = e->flags;
            send_present(p);
        });
        request_state_.connect(&o->events.request_state, [this](wlr_output_event_request_state* e) {
            events.request_state.emit(from_wlr_state(*e->state));
        });
        destroy_.connect(&o->events.destroy, [this](void*) { owner_.forget(this); });
    }

    ~WlrOutput() override {
        frame_.disconnect();
        needs_frame_.disconnect();
        damage_.disconnect();
        present_.disconnect();
        request_state_.disconnect();
        destroy_.disconnect();
    }

    // The wlroots state of `s`; false if it names a mode wlroots doesn't have.
    bool to_wlr_state(const OutputState& s, wlr_output_state* out) const {
        wlr_output_state_init(out);
        if (s.committed & OutputState::Enabled)
            wlr_output_state_set_enabled(out, s.enabled);
        if (s.committed & OutputState::ModeField) {
            if (s.mode_type == OutputState::ModeType::Fixed) {
                wlr_output_mode* m = s.mode ? reinterpret_cast<wlr_output_mode*>(s.mode->native) : nullptr;
                wlr_output_state_set_mode(out, m);
            } else {
                wlr_output_state_set_custom_mode(out, s.custom_mode.width, s.custom_mode.height,
                                                 s.custom_mode.refresh);
            }
        }
        if (s.committed & OutputState::Scale)
            wlr_output_state_set_scale(out, s.scale);
        if (s.committed & OutputState::Transform)
            wlr_output_state_set_transform(out, s.transform);
        if (s.committed & OutputState::AdaptiveSyncEnabled)
            wlr_output_state_set_adaptive_sync_enabled(out, s.adaptive_sync_enabled);
        if (s.committed & OutputState::Subpixel)
            wlr_output_state_set_subpixel(out, s.subpixel);
        // The render format is ours (it picks the swapchain): wlroots never
        // sees it, as it never renders for these outputs.
        if (s.committed & OutputState::Buffer) {
            wlr_output_state_set_buffer(out, proxy_for(s.buffer));
            out->buffer_src_box = to_wlr(s.buffer_src_box);
            out->buffer_dst_box = to_wlr(s.buffer_dst_box);
            out->tearing_page_flip = s.tearing_page_flip;
        }
        out->allow_reconfiguration = s.allow_reconfiguration;
        if (s.committed & OutputState::Damage)
            wlr_output_state_set_damage(out, &s.damage);
        if ((s.committed & OutputState::WaitTimeline) && s.wait_timeline)
            if (wlr_drm_syncobj_timeline* t = to_wlr(s.wait_timeline)) {
                wlr_output_state_set_wait_timeline(out, t, s.wait_point);
                wlr_drm_syncobj_timeline_unref(t);
            }
        if ((s.committed & OutputState::SignalTimeline) && s.signal_timeline)
            if (wlr_drm_syncobj_timeline* t = to_wlr(s.signal_timeline)) {
                wlr_output_state_set_signal_timeline(out, t, s.signal_point);
                wlr_drm_syncobj_timeline_unref(t);
            }
        if (s.committed & OutputState::ColorTransform)
            wlr_output_state_set_color_transform(out, s.color_transform);
        if (s.committed & OutputState::ImageDescriptionField) {
            if (s.image_description) {
                const wlr_output_image_description d = to_wlr(*s.image_description);
                wlr_output_state_set_image_description(out, &d);
            } else {
                wlr_output_state_set_image_description(out, nullptr);
            }
        }
        return true;
    }

    OutputState from_wlr_state(const wlr_output_state& w) const {
        OutputState s;
        if (w.committed & WLR_OUTPUT_STATE_ENABLED)
            s.set_enabled(w.enabled);
        if (w.committed & WLR_OUTPUT_STATE_MODE) {
            if (w.mode_type == WLR_OUTPUT_STATE_MODE_FIXED)
                s.set_mode(mode_of(w.mode));
            else
                s.set_custom_mode(w.custom_mode.width, w.custom_mode.height, w.custom_mode.refresh);
        }
        if (w.committed & WLR_OUTPUT_STATE_SCALE)
            s.set_scale(w.scale);
        if (w.committed & WLR_OUTPUT_STATE_TRANSFORM)
            s.set_transform(w.transform);
        if (w.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)
            s.set_adaptive_sync_enabled(w.adaptive_sync_enabled);
        return s;  // (a buffer in a request isn't ours to show)
    }

    const Mode* mode_of(const wlr_output_mode* m) const {
        auto it = std::ranges::find_if(modes, [m](const Mode& x) { return x.native == uint64_t(uintptr_t(m)); });
        return it == modes.end() ? nullptr : &*it;
    }

    // After wlroots took `s`: what it made of it (VRR may not have taken).
    void committed(const OutputState& s) {
        adaptive_sync_status = wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED ? AdaptiveSync::Enabled
                                                                                             : AdaptiveSync::Disabled;
        // finish_commit counts this commit; wlroots already has.
        seq_delta_ = commit_seq + 1 - wlr->commit_seq;
        finish_commit(s);
    }

    size_t gamma_size() const override { return wlr_output_get_gamma_size(wlr); }
    const FormatSet* primary_formats(uint32_t caps) const override {
        return wlr->impl->get_primary_formats ? own(wlr->impl->get_primary_formats(wlr, caps), primary_formats_)
                                              : nullptr;
    }
    bool direct_scanout_allowed() const override { return wlr_output_is_direct_scanout_allowed(wlr); }

    bool has_cursor_plane() const override { return wlr->impl->set_cursor != nullptr; }
    std::vector<std::pair<int, int>> cursor_sizes() const override {
        std::vector<std::pair<int, int>> out;
        if (wlr->impl->get_cursor_sizes) {
            size_t n = 0;
            const wlr_output_cursor_size* s = wlr->impl->get_cursor_sizes(wlr, &n);
            for (size_t i = 0; i < n; ++i)
                out.emplace_back(s[i].width, s[i].height);
        }
        return out;
    }
    const FormatSet* cursor_formats(uint32_t caps) const override {
        return wlr->impl->get_cursor_formats ? own(wlr->impl->get_cursor_formats(wlr, caps), cursor_formats_)
                                             : nullptr;
    }
    bool set_cursor(Buffer* b, int hx, int hy) override {
        if (!wlr->impl->set_cursor)
            return false;
        wlr_buffer* p = proxy_for(b);
        const bool ok = wlr->impl->set_cursor(wlr, p, hx, hy);
        let_go_unless_held(p);
        return ok;
    }
    bool move_cursor(int x, int y) override { return wlr->impl->move_cursor && wlr->impl->move_cursor(wlr, x, y); }

    wlr_output* const wlr;

private:
    mutable FormatSet primary_formats_, cursor_formats_;

protected:
    bool test(const OutputState& s) override {
        wlr_output_state st;
        to_wlr_state(s, &st);
        const bool ok = wlr_output_test_state(wlr, &st);
        wlr_output_state_finish(&st);
        return ok;
    }
    bool commit(const OutputState& s) override {
        wlr_output_state st;
        to_wlr_state(s, &st);
        const bool ok = wlr_output_commit_state(wlr, &st);
        wlr_output_state_finish(&st);
        if (ok) {
            adaptive_sync_status = wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED
                                       ? AdaptiveSync::Enabled
                                       : AdaptiveSync::Disabled;
            seq_delta_ = commit_seq + 1 - wlr->commit_seq;
        }
        return ok;
    }
    void schedule_frame_impl() override { wlr_output_schedule_frame(wlr); }

private:
    friend class WlrBackend;

    void sync_modes() {
        modes.clear();
        wlr_output_mode* m;
        wl_list_for_each(m, &wlr->modes, link) {
            modes.push_back({m->width, m->height, m->refresh, m->preferred, uint64_t(uintptr_t(m))});
        }
    }

    // Whatever wlroots set up before we saw it.
    void sync_fields() {
        enabled = wlr->enabled;
        width = wlr->width;
        height = wlr->height;
        refresh = wlr->refresh;
        scale = wlr->scale;
        transform = wlr->transform;
        subpixel = wlr->subpixel;
        current_mode = mode_of(wlr->current_mode);
        render_format = wlr->render_format;
        if (wlr->image_description)
            image_description = from_wlr(*wlr->image_description);
        commit_seq = wlr->commit_seq;
    }

    WlrBackend& owner_;
    uint32_t seq_delta_ = 0;
    Listener<> frame_, needs_frame_, destroy_;
    Listener<wlr_output_event_damage> damage_;
    Listener<wlr_output_event_present> present_;
    Listener<wlr_output_event_request_state> request_state_;
};

WlrBackend::WlrBackend(wl_event_loop* loop, wlr_backend* w) : Backend(loop), wlr_(w) {
    new_output_.connect(&w->events.new_output, [this](wlr_output* o) {
        auto* out = new WlrOutput(*this, o);
        outputs_.push_back(out);
        events.new_output.emit(out);
    });
    new_input_.connect(&w->events.new_input, [this](wlr_input_device* d) { events.new_input.emit(d); });
    // Nested, wlroots destroys it when the host session ends (its outputs
    // went first).
    wlr_destroy_.connect(&w->events.destroy, [this](void*) {
        new_output_.disconnect();
        new_input_.disconnect();
        wlr_destroy_.disconnect();
        wlr_ = nullptr;
    });
}

WlrBackend::~WlrBackend() {
    new_output_.disconnect();
    new_input_.disconnect();
    wlr_destroy_.disconnect();
    // Destroying the backend destroys its outputs, and with them ours.
    if (wlr_)
        wlr_backend_destroy(wlr_);
    for (WlrOutput* o : std::vector(outputs_))
        forget(o);
    events.destroy.emit();
}

void WlrBackend::forget(WlrOutput* o) {
    std::erase(outputs_, o);
    o->events.destroy.emit();
    delete o;
}

bool WlrBackend::start() {
    return wlr_backend_start(wlr_);
}

int WlrBackend::drm_fd() const {
    return wlr_backend_get_drm_fd(wlr_);
}

uint32_t WlrBackend::buffer_caps() const {
    return wlr_->buffer_caps;
}

bool WlrBackend::commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) {
    // Each output's own checks and blank modeset buffers, then all at once.
    std::vector<std::pair<WlrOutput*, OutputState>> ready;
    for (const auto& [o, st] : states) {
        ready.emplace_back(static_cast<WlrOutput*>(o), st);
        if (!o->prepare_commit(ready.back().second))
            return false;
    }
    std::vector<wlr_backend_output_state> wlr_states(ready.size());
    for (size_t i = 0; i < ready.size(); ++i) {
        wlr_states[i].output = ready[i].first->wlr;
        ready[i].first->to_wlr_state(ready[i].second, &wlr_states[i].base);
    }
    const bool ok = test_only ? wlr_backend_test(wlr_, wlr_states.data(), wlr_states.size())
                              : wlr_backend_commit(wlr_, wlr_states.data(), wlr_states.size());
    for (auto& st : wlr_states)
        wlr_output_state_finish(&st.base);
    if (ok && !test_only)
        for (auto& [o, st] : ready)
            o->committed(st);
    return ok;
}

namespace {

// The nested backend inside wlroots' multi backend, if it has one.
wlr_backend* nested_of(wlr_backend* b) {
    if (wlr_backend_is_wl(b))
        return b;
    wlr_backend* found = nullptr;
    if (wlr_backend_is_multi(b))
        wlr_multi_for_each_backend(
            b,
            [](wlr_backend* child, void* data) {
                if (wlr_backend_is_wl(child))
                    *static_cast<wlr_backend**>(data) = child;
            },
            &found);
    return found;
}

} // namespace

Output* WlrBackend::create_output() {
    // Nested: another window on the host. (Real screens can't be made.)
    wlr_backend* nested = nested_of(wlr_);
    wlr_output* o = nested ? wlr_wl_output_create(nested) : nullptr;
    return o ? output_of(o) : nullptr;
}

bool WlrBackend::is_virtual(const Output* o) const {
    auto it = std::ranges::find(outputs_, o);
    return it != outputs_.end() && wlr_output_is_wl((*it)->wlr);
}

bool WlrBackend::destroy_output(Output* o) {
    if (!is_virtual(o))
        return false;
    wlr_output_destroy(static_cast<WlrOutput*>(o)->wlr);  // takes ours with it
    return true;
}

bool WlrBackend::is_drm() const {
    bool drm = wlr_backend_is_drm(wlr_);
    if (wlr_backend_is_multi(wlr_))
        wlr_multi_for_each_backend(
            wlr_, [](wlr_backend* b, void* data) { *static_cast<bool*>(data) |= wlr_backend_is_drm(b); }, &drm);
    return drm;
}

Output* WlrBackend::output_of(wlr_output* o) const {
    auto it = std::ranges::find_if(outputs_, [o](const WlrOutput* w) { return w->wlr == o; });
    return it == outputs_.end() ? nullptr : *it;
}

} // namespace atrium::backend
