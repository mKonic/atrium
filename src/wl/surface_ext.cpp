#include "wl/surface_ext.hpp"

#include "alpha-modifier-v1-server.hpp"
#include "content-type-v1-server.hpp"
#include "fractional-scale-v1-server.hpp"
#include "single-pixel-buffer-v1-server.hpp"
#include "tearing-control-v1-server.hpp"
#include "viewporter-server.hpp"

#include <drm_fourcc.h>

#include <cmath>

namespace atrium::wl {

namespace {

// Registers `object` as `surface`'s one object of a kind in `map`, erroring
// on a second. Its going (or the surface's) takes it out again, and `reset`
// undoes its state from the surface's next commit.
template <class T>
bool track(std::map<Surface*, Resource*>& map, Surface* surface, T* object, Resource* on, uint32_t exists_error,
           std::function<void(SurfaceState&)> reset) {
    if (map.contains(surface)) {
        on->post_error(exists_error, "the surface already has one");
        object->detach();
        return false;
    }
    map[surface] = object;
    auto gone = std::make_shared<Connection>();
    *gone = surface->events.destroy.connect([&map, surface, object] {
        if (map[surface] == object)
            map.erase(surface);
        object->detach();
    });
    object->on_gone([&map, surface, object, gone, reset] {
        gone->disconnect();
        if (auto it = map.find(surface); it != map.end() && it->second == object) {
            map.erase(it);
            if (reset)
                reset(surface->pending_state());
        }
    });
    return true;
}

template <class Map>
void detach_all(Map& map) {
    for (auto& [surface, r] : map)
        r->detach();
    map.clear();
}

} // namespace

// ---- viewporter -----------------------------------------------------------------

Viewporter::Viewporter(wl_display* display) {
    global_ = Global::create<WpViewporter>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<WpViewporter>(client, version, id);
        if (!m)
            return;
        m->on_get_viewport([this](WpViewporter* self, uint32_t id, wl_resource* surface_res) {
            auto* v = make<WpViewport>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!v)
                return;
            if (!s) {
                v->detach();
                return;
            }
            if (!track(viewports_, s, v, self, uint32_t(WpViewporter::Error::ViewportExists),
                       [](SurfaceState& p) {
                           p.viewport = {};
                           p.committed |= SurfaceState::Viewport;
                       }))
                return;
            auto viewport_of = [s]() -> SurfaceState::ViewportState& {
                s->pending_state().committed |= SurfaceState::Viewport;
                return s->pending_state().viewport;
            };
            v->on_set_source([v, viewport_of](WpViewport*, double x, double y, double w, double h) {
                if (x == -1 && y == -1 && w == -1 && h == -1) {
                    viewport_of().has_source = false;
                    return;
                }
                if (x < 0 || y < 0 || w <= 0 || h <= 0) {
                    v->post_error(uint32_t(WpViewport::Error::BadValue), "a bad source rectangle");
                    return;
                }
                auto& vp = viewport_of();
                vp.has_source = true;
                vp.sx = x;
                vp.sy = y;
                vp.sw = w;
                vp.sh = h;
            });
            v->on_set_destination([v, viewport_of](WpViewport*, int32_t w, int32_t h) {
                if (w == -1 && h == -1) {
                    viewport_of().has_destination = false;
                    return;
                }
                if (w <= 0 || h <= 0) {
                    v->post_error(uint32_t(WpViewport::Error::BadValue), "a bad destination size");
                    return;
                }
                auto& vp = viewport_of();
                vp.has_destination = true;
                vp.dw = w;
                vp.dh = h;
            });
            // Checked as the commit goes in: against the buffer it will show.
            auto check = std::make_shared<Connection>();
            *check = s->events.queued.connect([v, s](SurfaceState* st) {
                if (v->inert())
                    return;
                const auto& vp = st->viewport;
                if (!vp.has_source)
                    return;
                if (!vp.has_destination && (vp.sw != std::floor(vp.sw) || vp.sh != std::floor(vp.sh))) {
                    v->post_error(uint32_t(WpViewport::Error::BadSize), "a fractional source needs a destination");
                    st->rejected = true;
                    return;
                }
                wlr_buffer* b = (st->committed & SurfaceState::Buffer) ? st->buffer.get() : s->current().buffer.get();
                int bw = b ? b->width : s->current().buffer_width, bh = b ? b->height : s->current().buffer_height;
                if (st->transform & WL_OUTPUT_TRANSFORM_90)
                    std::swap(bw, bh);
                const double w = double(bw) / std::max(1, st->scale), h = double(bh) / std::max(1, st->scale);
                if (bw > 0 && (vp.sx + vp.sw > w + 1e-3 || vp.sy + vp.sh > h + 1e-3)) {
                    v->post_error(uint32_t(WpViewport::Error::OutOfBuffer), "the source is outside the buffer");
                    st->rejected = true;
                }
            });
            v->on_destroy([check](WpViewport*) { check->disconnect(); });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

Viewporter::~Viewporter() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    detach_all(viewports_);
}

// ---- fractional scale ------------------------------------------------------------------

FractionalScales::FractionalScales(wl_display* display) {
    global_ = Global::create<WpFractionalScaleManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                            uint32_t id) {
        auto* m = make<WpFractionalScaleManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_fractional_scale([this](WpFractionalScaleManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* f = make<WpFractionalScaleV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!f)
                return;
            if (!s) {
                f->detach();
                return;
            }
            if (!track(scales_, s, f, self, uint32_t(WpFractionalScaleManagerV1::Error::FractionalScaleExists), {}))
                return;
            if (auto it = preferred_.find(s); it != preferred_.end())
                f->send_preferred_scale(uint32_t(std::lround(it->second * 120)));
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

FractionalScales::~FractionalScales() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    detach_all(scales_);
}

void FractionalScales::set_preferred_scale(Surface* surface, double scale) {
    auto [it, fresh] = preferred_.try_emplace(surface, scale);
    if (!fresh && it->second == scale)
        return;
    it->second = scale;
    if (fresh) {
        auto gone = std::make_shared<Connection>();
        *gone = surface->events.destroy.connect([this, surface, gone] {
            preferred_.erase(surface);
            gone->disconnect();
        });
    }
    if (auto s = scales_.find(surface); s != scales_.end())
        static_cast<WpFractionalScaleV1*>(s->second)->send_preferred_scale(uint32_t(std::lround(scale * 120)));
}

// ---- alpha, content type, tearing -----------------------------------------------------

SurfaceHints::SurfaceHints(wl_display* display) {
    alpha_ = Global::create<WpAlphaModifierV1>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<WpAlphaModifierV1>(client, version, id);
        if (!m)
            return;
        m->on_get_surface([this](WpAlphaModifierV1* self, uint32_t id, wl_resource* surface_res) {
            auto* a = make<WpAlphaModifierSurfaceV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!a)
                return;
            if (!s) {
                a->detach();
                return;
            }
            if (!track(alphas_, s, a, self, uint32_t(WpAlphaModifierV1::Error::AlreadyConstructed),
                       [](SurfaceState& p) {
                           p.alpha = 1;
                           p.committed |= SurfaceState::Alpha;
                       }))
                return;
            a->on_set_multiplier([s](WpAlphaModifierSurfaceV1*, uint32_t factor) {
                s->pending_state().alpha = float(double(factor) / 0xffffffffu);
                s->pending_state().committed |= SurfaceState::Alpha;
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    content_ = Global::create<WpContentTypeManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                         uint32_t id) {
        auto* m = make<WpContentTypeManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_surface_content_type([this](WpContentTypeManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* c = make<WpContentTypeV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!c)
                return;
            if (!s) {
                c->detach();
                return;
            }
            if (!track(contents_, s, c, self, uint32_t(WpContentTypeManagerV1::Error::AlreadyConstructed),
                       [](SurfaceState& p) {
                           p.content_type = 0;
                           p.committed |= SurfaceState::ContentType;
                       }))
                return;
            c->on_set_content_type([s](WpContentTypeV1*, uint32_t type) {
                if (type > 3)
                    return;
                s->pending_state().content_type = type;
                s->pending_state().committed |= SurfaceState::ContentType;
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
    tearing_ = Global::create<WpTearingControlManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                           uint32_t id) {
        auto* m = make<WpTearingControlManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_tearing_control([this](WpTearingControlManagerV1* self, uint32_t id, wl_resource* surface_res) {
            auto* t = make<WpTearingControlV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!t)
                return;
            if (!s) {
                t->detach();
                return;
            }
            if (!track(tearings_, s, t, self, uint32_t(WpTearingControlManagerV1::Error::TearingControlExists),
                       [](SurfaceState& p) {
                           p.presentation_hint = 0;
                           p.committed |= SurfaceState::Tearing;
                       }))
                return;
            t->on_set_presentation_hint([s](WpTearingControlV1*, uint32_t hint) {
                s->pending_state().presentation_hint = hint;
                s->pending_state().committed |= SurfaceState::Tearing;
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

SurfaceHints::~SurfaceHints() {
    alpha_.reset();
    content_.reset();
    tearing_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
    detach_all(alphas_);
    detach_all(contents_);
    detach_all(tearings_);
}

// ---- single pixel buffers ---------------------------------------------------------------

namespace {

struct PixelStorage {
    wlr_buffer base;
    uint32_t rgba[4];  // straight, 0..UINT32_MAX
    uint8_t argb8888[4];  // premultiplied, as a 1x1 ARGB8888 image (b, g, r, a)
};

const wlr_buffer_impl kPixelImpl = {
    .destroy =
        [](wlr_buffer* b) {
            wlr_buffer_finish(b);
            delete reinterpret_cast<PixelStorage*>(b);
        },
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access =
        [](wlr_buffer* b, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
            if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
                return false;
            *data = reinterpret_cast<PixelStorage*>(b)->argb8888;
            *format = DRM_FORMAT_ARGB8888;
            *stride = 4;
            return true;
        },
    .end_data_ptr_access = [](wlr_buffer*) {},
};

class PixelBuffer : public ClientBuffer {
public:
    PixelBuffer(wl_client* client, uint32_t version, uint32_t id, PixelStorage* s)
        : ClientBuffer(client, version, id, &s->base) {}
};

} // namespace

SinglePixelBuffers::SinglePixelBuffers(wl_display* display) {
    global_ = Global::create<WpSinglePixelBufferManagerV1>(display, 1, [this](wl_client* client, uint32_t version,
                                                                              uint32_t id) {
        auto* m = make<WpSinglePixelBufferManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_create_u32_rgba_buffer([](WpSinglePixelBufferManagerV1* self, uint32_t id, uint32_t r, uint32_t g,
                                        uint32_t b, uint32_t a) {
            auto* s = new PixelStorage{{}, {r, g, b, a}, {}};
            // The protocol's values are premultiplied already.
            s->argb8888[0] = uint8_t(b >> 24);
            s->argb8888[1] = uint8_t(g >> 24);
            s->argb8888[2] = uint8_t(r >> 24);
            s->argb8888[3] = uint8_t(a >> 24);
            wlr_buffer_init(&s->base, &kPixelImpl, 1, 1);
            if (!make<PixelBuffer>(self->client(), 1, id, s))
                wlr_buffer_drop(&s->base);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

SinglePixelBuffers::~SinglePixelBuffers() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
}

bool SinglePixelBuffers::color_of(wlr_buffer* buffer, float rgba[4]) {
    if (!buffer || buffer->impl != &kPixelImpl)
        return false;
    const auto* s = reinterpret_cast<PixelStorage*>(buffer);
    for (int i = 0; i < 4; ++i)
        rgba[i] = float(double(s->rgba[i]) / 0xffffffffu);
    return true;
}

} // namespace atrium::wl
