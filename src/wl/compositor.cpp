#include "wl/compositor.hpp"

#include "wl/output.hpp"

#include <algorithm>
#include <cmath>

namespace atrium::wl {

namespace {

// Surface size from buffer size: transform, then scale, then the viewport.
void size_state(SurfaceState& s) {
    wlr_buffer* b = s.buffer.get();
    s.buffer_width = b ? b->width : 0;
    s.buffer_height = b ? b->height : 0;
    int w = s.buffer_width, h = s.buffer_height;
    if (s.transform & WL_OUTPUT_TRANSFORM_90)
        std::swap(w, h);
    w /= std::max(1, s.scale);
    h /= std::max(1, s.scale);
    if (!b) {
        s.width = s.height = 0;
        return;
    }
    if (s.viewport.has_destination) {
        w = s.viewport.dw;
        h = s.viewport.dh;
    } else if (s.viewport.has_source) {
        w = int(s.viewport.sw);
        h = int(s.viewport.sh);
    }
    s.width = w;
    s.height = h;
}

const wlr_buffer_impl kSurfaceBufferImpl = {
    .destroy =
        [](wlr_buffer* b) {
            auto* sb = reinterpret_cast<SurfaceBuffer*>(b);
            if (sb->texture)
                wlr_texture_destroy(sb->texture);
            wlr_buffer_finish(b);
            delete sb;
        },
    .get_dmabuf =
        [](wlr_buffer* b, wlr_dmabuf_attributes* out) {
            auto* sb = reinterpret_cast<SurfaceBuffer*>(b);
            return sb->source && wlr_buffer_get_dmabuf(sb->source.get(), out);
        },
    .get_shm = nullptr,
    .begin_data_ptr_access = nullptr,
    .end_data_ptr_access = nullptr,
};

} // namespace

SurfaceBuffer* SurfaceBuffer::from(wlr_buffer* buffer) {
    return buffer && buffer->impl == &kSurfaceBufferImpl ? reinterpret_cast<SurfaceBuffer*>(buffer) : nullptr;
}

// ---- SurfaceState ---------------------------------------------------------------

void SurfaceState::merge(SurfaceState&& later) {
    if (later.committed & Buffer) {
        buffer = std::move(later.buffer);
        // Offsets add up: each moved the content by its own amount.
        dx += later.dx;
        dy += later.dy;
    } else if (later.committed & Offset) {
        dx += later.dx;
        dy += later.dy;
    }
    if (later.committed & Damage) {
        surface_damage.add(later.surface_damage);
        buffer_damage.add(later.buffer_damage);
    }
    if (later.committed & Opaque)
        opaque = std::move(later.opaque);
    if (later.committed & Input)
        input = std::move(later.input);
    if (later.committed & Transform)
        transform = later.transform;
    if (later.committed & Scale)
        scale = later.scale;
    if (later.committed & Frame)
        for (auto& f : later.frames)
            frames.push_back(std::move(f));
    if (later.committed & Viewport)
        viewport = later.viewport;
    if (later.committed & Subsurfaces)
        subsurfaces = std::move(later.subsurfaces);
    committed |= later.committed;
}

// ---- Surface ----------------------------------------------------------------------

Surface::Surface(wl_client* client, uint32_t version, uint32_t id, Compositor& compositor)
    : WlSurface(client, version, id), compositor_(compositor) {
    on_attach([this](WlSurface*, WlBuffer* buffer_resource, int32_t x, int32_t y) {
        if (this->version() >= 5 && (x != 0 || y != 0)) {
            post_error(uint32_t(Error::InvalidOffset), "attach with an offset; use wl_surface.offset");
            return;
        }
        wlr_buffer* buffer = nullptr;
        if (buffer_resource) {
            auto* b = dynamic_cast<ClientBuffer*>(buffer_resource);
            if (!b) {
                post_error(0, "unknown kind of buffer");  // wl_display.invalid_object
                return;
            }
            buffer = b->buffer();
        }
        pending_.buffer.reset(buffer);
        pending_.committed |= SurfaceState::Buffer;
        if (x || y) {
            pending_.dx = x;
            pending_.dy = y;
            pending_.committed |= SurfaceState::Offset;
        }
    });
    on_damage([this](WlSurface*, int32_t x, int32_t y, int32_t w, int32_t h) {
        pending_.surface_damage.add(x, y, w, h);
        pending_.committed |= SurfaceState::Damage;
    });
    on_damage_buffer([this](WlSurface*, int32_t x, int32_t y, int32_t w, int32_t h) {
        pending_.buffer_damage.add(x, y, w, h);
        pending_.committed |= SurfaceState::Damage;
    });
    on_frame([this](WlSurface*, uint32_t id) {
        if (auto* cb = make<WlCallback>(this->client(), 1, id)) {
            pending_.frames.push_back(cb);
            pending_.committed |= SurfaceState::Frame;
        }
    });
    on_set_opaque_region([this](WlSurface*, WlRegion* region) {
        auto* r = dynamic_cast<RegionResource*>(region);
        pending_.opaque = r ? r->region : Region();
        pending_.committed |= SurfaceState::Opaque;
    });
    on_set_input_region([this](WlSurface*, WlRegion* region) {
        auto* r = dynamic_cast<RegionResource*>(region);
        pending_.input = r ? r->region : Region::infinite();
        pending_.committed |= SurfaceState::Input;
    });
    on_set_buffer_transform([this](WlSurface*, int32_t transform) {
        if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
            post_error(uint32_t(Error::InvalidTransform), "no such transform");
            return;
        }
        pending_.transform = transform;
        pending_.committed |= SurfaceState::Transform;
    });
    on_set_buffer_scale([this](WlSurface*, int32_t scale) {
        if (scale <= 0) {
            post_error(uint32_t(Error::InvalidScale), "the scale must be positive");
            return;
        }
        pending_.scale = scale;
        pending_.committed |= SurfaceState::Scale;
    });
    on_offset([this](WlSurface*, int32_t x, int32_t y) {
        pending_.dx = x;
        pending_.dy = y;
        pending_.committed |= SurfaceState::Offset;
    });
    on_commit([this](WlSurface*) { commit(); });
}

Surface::~Surface() {
    if (mapped_)
        unmap();
    events.destroy.emit();
    // Children stay, unmapped, with no parent.
    for (const auto& p : std::vector(current_.subsurfaces))
        if (p.sub)
            p.sub->detach_from_parent();
    for (const auto& p : std::vector(pending_.subsurfaces))
        if (p.sub)
            p.sub->detach_from_parent();
    queue_.clear();
    if (shown_)
        wlr_buffer_drop(&shown_->base);
}

Surface* Surface::from(wl_resource* resource) {
    return dynamic_cast<Surface*>(WlSurface::from(resource));
}

bool Surface::set_role(Role* role, Resource* on, uint32_t error) {
    if ((role_name_ && role_name_ != role->name()) || role_) {
        if (on)
            on->post_error(error, role_ ? "the surface already has a role object" : "the surface has another role");
        return false;
    }
    role_name_ = role->name();
    role_ = role;
    return true;
}

void Surface::clear_role(Role* role) {
    if (role_ == role)
        role_ = nullptr;
}

void Surface::map() {
    if (mapped_)
        return;
    mapped_ = true;
    events.map.emit();
}

void Surface::unmap() {
    if (!mapped_)
        return;
    mapped_ = false;
    events.unmap.emit();
}

Surface* Surface::root() {
    Surface* s = this;
    while (s->subsurface_ && s->subsurface_->parent())
        s = s->subsurface_->parent();
    return s;
}

bool Surface::synchronized() const {
    for (const Surface* s = this; s->subsurface_; s = s->subsurface_->parent()) {
        if (s->subsurface_->sync())
            return true;
        if (!s->subsurface_->parent())
            break;
    }
    return false;
}

void Surface::commit() {
    if (role_ && !role_->precommit(*this))
        return;
    events.precommit.emit();
    if (pending_.buffer && !pending_.viewport.has_destination && pending_.scale > 1) {
        wlr_buffer* b = pending_.buffer.get();
        if (b->width % pending_.scale || b->height % pending_.scale) {
            post_error(uint32_t(Error::InvalidSize), "the buffer's size isn't a multiple of its scale");
            return;
        }
    }
    // The pending order of subsurfaces is edited in place (added, placed),
    // so it carries over to the next commit.
    std::vector<SurfaceState::Placement> order = pending_.subsurfaces;
    auto next = std::make_unique<SurfaceState>();
    next->merge(std::move(pending_));
    pending_ = SurfaceState{};
    pending_.subsurfaces = std::move(order);
    if (synchronized())
        next->locks |= SurfaceState::LockSync;
    queue_.push_back(std::move(next));
    process_queue();
}

void Surface::unlock(SurfaceState* state, uint32_t lock) {
    for (auto& s : queue_)
        if (s.get() == state)
            s->locks &= ~lock;
    process_queue();
}

void Surface::process_queue() {
    while (!queue_.empty()) {
        SurfaceState& front = *queue_.front();
        if (front.locks)
            break;
        // Several ready at once apply as one.
        if (queue_.size() > 1 && !queue_[1]->locks) {
            SurfaceState later = std::move(*queue_[1]);
            queue_.erase(queue_.begin() + 1);
            front.merge(std::move(later));
            continue;
        }
        std::unique_ptr<SurfaceState> state = std::move(queue_.front());
        queue_.pop_front();
        apply(*state);
    }
}

void Surface::apply(SurfaceState& s) {
    const bool buffer_changed = s.committed & SurfaceState::Buffer;
    if (buffer_changed) {
        current_.buffer = std::move(s.buffer);
        current_.dx = s.dx;
        current_.dy = s.dy;
    } else if (s.committed & SurfaceState::Offset) {
        current_.dx = s.dx;
        current_.dy = s.dy;
    } else {
        current_.dx = current_.dy = 0;
    }
    if (s.committed & SurfaceState::Opaque)
        current_.opaque = std::move(s.opaque);
    if (s.committed & SurfaceState::Input)
        current_.input = std::move(s.input);
    if (s.committed & SurfaceState::Transform)
        current_.transform = s.transform;
    if (s.committed & SurfaceState::Scale)
        current_.scale = s.scale;
    if (s.committed & SurfaceState::Viewport)
        current_.viewport = s.viewport;
    if (s.committed & SurfaceState::Frame)
        for (auto& f : s.frames)
            current_.frames.push_back(std::move(f));
    if (s.committed & SurfaceState::Subsurfaces)
        current_.subsurfaces = std::move(s.subsurfaces);
    current_.committed = s.committed;
    const int old_w = current_.buffer_width, old_h = current_.buffer_height;
    size_state(current_);

    // This commit's damage, in buffer pixels. Surface damage under a scale
    // or transform or crop is taken as the whole buffer: correct, if more.
    current_.buffer_damage = std::move(s.buffer_damage);
    if (!s.surface_damage.empty()) {
        if (current_.transform == WL_OUTPUT_TRANSFORM_NORMAL && !current_.viewport.has_source &&
            !current_.viewport.has_destination) {
            Region d = s.surface_damage;
            if (current_.scale != 1) {
                int n = 0;
                const pixman_box32_t* rects = pixman_region32_rectangles(d.get(), &n);
                Region scaled;
                const int k = current_.scale;
                for (int i = 0; i < n; ++i)
                    scaled.add(rects[i].x1 * k, rects[i].y1 * k, (rects[i].x2 - rects[i].x1) * k,
                               (rects[i].y2 - rects[i].y1) * k);
                d = std::move(scaled);
            }
            current_.buffer_damage.add(d);
        } else {
            current_.buffer_damage.add(0, 0, current_.buffer_width, current_.buffer_height);
        }
    }
    if (buffer_changed && (old_w != current_.buffer_width || old_h != current_.buffer_height))
        current_.buffer_damage.add(0, 0, current_.buffer_width, current_.buffer_height);
    current_.buffer_damage.intersect(0, 0, current_.buffer_width, current_.buffer_height);

    if (buffer_changed)
        update_texture(true);

    // Subsurfaces move with the commit that placed them.
    for (const auto& p : current_.subsurfaces)
        if (p.sub)
            p.sub->update_mapped();

    if (role_)
        role_->commit(*this);
    events.commit.emit();
    release_children();
}

// Synchronized children's commits wait for their parent's: now.
void Surface::release_children() {
    for (const auto& p : std::vector(current_.subsurfaces))
        if (p.sub && p.sub->surface()) {
            Surface* child = p.sub->surface();
            for (auto& q : child->queue_)
                q->locks &= ~SurfaceState::LockSync;
            child->process_queue();
        }
}

void Surface::update_texture(bool) {
    wlr_buffer* source = current_.buffer.get();
    if (!source) {
        if (shown_) {
            wlr_buffer_drop(&shown_->base);
            shown_ = nullptr;
        }
        return;
    }
    wlr_renderer* renderer = compositor_.renderer();
    // An shm buffer of the size and format shown is copied into the texture
    // we have, only where it changed.
    void* data;
    uint32_t format;
    size_t stride;
    const bool in_memory = wlr_buffer_begin_data_ptr_access(source, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data,
                                                              &format, &stride);
    if (in_memory)
        wlr_buffer_end_data_ptr_access(source);
    if (in_memory && shown_ && shown_->texture && shown_->base.n_locks == shown_->ignore_locks &&
        shown_->texture->width == uint32_t(source->width) && shown_->texture->height == uint32_t(source->height) &&
        wlr_texture_update_from_buffer(shown_->texture, source, current_.buffer_damage.get())) {
        current_.buffer.reset();  // copied: back to the client
        return;
    }
    auto* sb = new SurfaceBuffer{};
    wlr_buffer_init(&sb->base, &kSurfaceBufferImpl, source->width, source->height);
    sb->texture = renderer ? wlr_texture_from_buffer(renderer, source) : nullptr;
    if (in_memory && sb->texture)
        current_.buffer.reset();  // copied: back to the client
    else
        sb->source.reset(source);
    if (shown_)
        wlr_buffer_drop(&shown_->base);
    shown_ = sb;
}

wlr_fbox Surface::source_box() const {
    const auto& v = current_.viewport;
    if (v.has_source)
        return {v.sx * current_.scale, v.sy * current_.scale, v.sw * current_.scale, v.sh * current_.scale};
    return {0, 0, double(current_.buffer_width), double(current_.buffer_height)};
}

bool Surface::accepts_input(double sx, double sy) const {
    return sx >= 0 && sy >= 0 && sx < current_.width && sy < current_.height &&
           current_.input.contains(int(std::floor(sx)), int(std::floor(sy)));
}

void Surface::send_frame_done(uint32_t ms) {
    for (auto& f : std::exchange(current_.frames, {}))
        if (WlCallback* cb = f.get()) {
            cb->send_done(ms);
            cb->destroy();
        }
}

void Surface::enter(Output& output) {
    if (std::ranges::find(outputs_, &output) != outputs_.end())
        return;
    outputs_.push_back(&output);
    for (WlOutput* r : output.resources_for(client()))
        send_enter(r);
}

void Surface::leave(Output& output) {
    if (!std::erase(outputs_, &output))
        return;
    for (WlOutput* r : output.resources_for(client()))
        send_leave(r);
}

void Surface::set_preferred_scale(int32_t scale) {
    if (scale == preferred_scale_)
        return;
    preferred_scale_ = scale;
    send_preferred_buffer_scale(scale);
}

void Surface::set_preferred_transform(uint32_t transform) {
    if (transform == preferred_transform_)
        return;
    preferred_transform_ = transform;
    send_preferred_buffer_transform(transform);
}

// ---- Subsurface -----------------------------------------------------------------

Subsurface::Subsurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, Surface* parent)
    : WlSubsurface(client, version, id), surface_(surface), parent_(parent) {
    surface->subsurface_ = this;
    // New subsurfaces go on top, from the parent's next commit.
    parent->pending_.subsurfaces.push_back({this, 0, 0});
    parent->pending_.committed |= SurfaceState::Subsurfaces;
    parent_map_ = parent->events.map.connect([this] { update_mapped(); });
    parent_unmap_ = parent->events.unmap.connect([this] { update_mapped(); });
    // Its surface gone, the subsurface is inert until the client destroys it.
    surface_destroy_ = surface->events.destroy.connect([this] {
        detach_from_parent();
        surface_ = nullptr;
    });

    on_set_position([this](WlSubsurface*, int32_t x, int32_t y) {
        if (!parent_)
            return;
        for (auto& p : parent_->pending_.subsurfaces)
            if (p.sub == this) {
                p.x = x;
                p.y = y;
            }
        parent_->pending_.committed |= SurfaceState::Subsurfaces;
    });
    on_place_above([this](WlSubsurface*, WlSurface* sibling) { place(dynamic_cast<Surface*>(sibling), true); });
    on_place_below([this](WlSubsurface*, WlSurface* sibling) { place(dynamic_cast<Surface*>(sibling), false); });
    on_set_sync([this](WlSubsurface*) { sync_ = true; });
    on_set_desync([this](WlSubsurface*) {
        if (!sync_)
            return;
        sync_ = false;
        // What waited on the parent goes now, unless an ancestor still syncs.
        if (surface_ && !surface_->synchronized()) {
            for (auto& q : surface_->queue_)
                q->locks &= ~SurfaceState::LockSync;
            surface_->process_queue();
        }
    });
}

Subsurface::~Subsurface() {
    detach_from_parent();
    if (surface_) {
        surface_->unmap();
        surface_->subsurface_ = nullptr;
        surface_->clear_role(this);
    }
}

void Subsurface::detach_from_parent() {
    if (!parent_)
        return;
    for (SurfaceState* s : {&parent_->pending_, &parent_->current_})
        std::erase_if(s->subsurfaces, [this](const auto& p) { return p.sub == this; });
    for (auto& q : parent_->queue_)
        std::erase_if(q->subsurfaces, [this](const auto& p) { return p.sub == this; });
    parent_ = nullptr;
    parent_map_.disconnect();
    parent_unmap_.disconnect();
    if (surface_)
        surface_->unmap();
}

void Subsurface::place(Surface* sibling, bool above) {
    if (!parent_)
        return;
    auto& order = parent_->pending_.subsurfaces;
    auto is_sibling = [&](const SurfaceState::Placement& p) {
        return sibling == parent_ ? p.sub == nullptr : (p.sub && p.sub->surface() == sibling);
    };
    if (!sibling || sibling == surface_ || std::ranges::find_if(order, is_sibling) == order.end()) {
        post_error(uint32_t(Error::BadSurface), "not a sibling or the parent");
        return;
    }
    auto self = std::ranges::find_if(order, [this](const auto& p) { return p.sub == this; });
    if (self == order.end())
        return;
    SurfaceState::Placement me = *self;
    order.erase(self);
    auto at = std::ranges::find_if(order, is_sibling);
    order.insert(above ? at + 1 : at, me);
    parent_->pending_.committed |= SurfaceState::Subsurfaces;
}

int Subsurface::x() const {
    if (parent_)
        for (const auto& p : parent_->current_.subsurfaces)
            if (p.sub == this)
                return p.x;
    return 0;
}

int Subsurface::y() const {
    if (parent_)
        for (const auto& p : parent_->current_.subsurfaces)
            if (p.sub == this)
                return p.y;
    return 0;
}

void Subsurface::commit(Surface&) {
    update_mapped();
}

// Shown when it has content, its parent has taken it (committed after it was
// made) and the parent is mapped.
void Subsurface::update_mapped() {
    if (!surface_)
        return;
    bool placed = false;
    if (parent_)
        for (const auto& p : parent_->current_.subsurfaces)
            placed |= p.sub == this;
    if (parent_ && placed && parent_->mapped() && surface_->current().buffer_width > 0)
        surface_->map();
    else
        surface_->unmap();
}

// ---- Region ----------------------------------------------------------------------

RegionResource::RegionResource(wl_client* client, uint32_t version, uint32_t id) : WlRegion(client, version, id) {
    on_add([this](WlRegion*, int32_t x, int32_t y, int32_t w, int32_t h) { region.add(x, y, w, h); });
    on_subtract([this](WlRegion*, int32_t x, int32_t y, int32_t w, int32_t h) { region.subtract(x, y, w, h); });
}

// ---- Compositor ------------------------------------------------------------------

Compositor::Compositor(wl_display* display, wlr_renderer* renderer) : renderer_(renderer) {
    compositor_global_ = Global::create<WlCompositor>(display, 6, [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* c = make<WlCompositor>(client, version, id);
            if (!c)
                return;
            c->on_create_surface([this](WlCompositor* self, uint32_t id) {
                if (auto* s = make<Surface>(self->client(), self->version(), id, *this))
                    new_surface.emit(s);
            });
            c->on_create_region([](WlCompositor* self, uint32_t id) {
                make<RegionResource>(self->client(), 1, id);
            });
            std::erase_if(compositors_, [](const auto& w) { return !w; });
            compositors_.push_back(c);
        });
    subcompositor_global_ = Global::create<WlSubcompositor>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
            auto* c = make<WlSubcompositor>(client, version, id);
            if (!c)
                return;
            c->on_get_subsurface([](WlSubcompositor* self, uint32_t id, WlSurface* surface_resource,
                                    WlSurface* parent_resource) {
                auto* surface = dynamic_cast<Surface*>(surface_resource);
                auto* parent = dynamic_cast<Surface*>(parent_resource);
                if (!surface || !parent)
                    return;
                // No loops: the parent can't be the surface or below it.
                for (Surface* s = parent; s; s = s->subsurface() ? s->subsurface()->parent() : nullptr)
                    if (s == surface) {
                        self->post_error(uint32_t(WlSubcompositor::Error::BadParent),
                                         "the parent is the surface or one of its subsurfaces");
                        return;
                    }
                if ((surface->role_name() && surface->role_name() != Subsurface::kRole) || surface->role()) {
                    self->post_error(uint32_t(WlSubcompositor::Error::BadSurface), "the surface has a role");
                    return;
                }
                auto* sub = make<Subsurface>(self->client(), self->version(), id, surface, parent);
                if (!sub)
                    return;
                surface->set_role(sub, nullptr, 0);
                parent->events.new_subsurface.emit(sub);
            });
            std::erase_if(subcompositors_, [](const auto& w) { return !w; });
            subcompositors_.push_back(c);
        });
}

Compositor::~Compositor() {
    compositor_global_.reset();
    subcompositor_global_.reset();
    for (auto& c : compositors_)
        if (c)
            c->detach();
    for (auto& c : subcompositors_)
        if (c)
            c->detach();
}

} // namespace atrium::wl
