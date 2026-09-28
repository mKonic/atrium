// Layer shell; the validation follows wlroots' types/wlr_layer_shell_v1.c.
#include "wl/layer_shell.hpp"

#include "wl/output.hpp"
#include "wl/xdg_shell.hpp"

#include <algorithm>

namespace atrium::wl {

namespace {

constexpr uint32_t kTop = 1, kBottom = 2, kLeft = 4, kRight = 8;

} // namespace

LayerSurface::LayerSurface(wl_client* client, uint32_t version, uint32_t id, Surface* surface, Output* output,
                           uint32_t layer, std::string name_space)
    : ZwlrLayerSurfaceV1(client, version, id), surface_(surface), output_(output),
      namespace_(std::move(name_space)) {
    surface->pending_state().layer.layer = layer;
    surface->pending_state().committed |= SurfaceState::Layer;
    surface_gone_ = surface->events.destroy.connect([this] {
        reset();
        gone();
        surface_ = nullptr;
    });

    auto pending = [this]() -> State* {
        if (!surface_)
            return nullptr;
        surface_->pending_state().committed |= SurfaceState::Layer;
        return &surface_->pending_state().layer;
    };
    on_set_size([pending](ZwlrLayerSurfaceV1*, uint32_t w, uint32_t h) {
        if (State* p = pending()) {
            p->desired_width = w;
            p->desired_height = h;
        }
    });
    on_set_anchor([this, pending](ZwlrLayerSurfaceV1*, uint32_t anchor) {
        if (anchor > (kTop | kBottom | kLeft | kRight)) {
            post_error(uint32_t(Error::InvalidAnchor), "no such anchor");
            return;
        }
        if (State* p = pending())
            p->anchor = anchor;
    });
    on_set_exclusive_zone([pending](ZwlrLayerSurfaceV1*, int32_t zone) {
        if (State* p = pending())
            p->exclusive_zone = zone;
    });
    on_set_margin([pending](ZwlrLayerSurfaceV1*, int32_t top, int32_t right, int32_t bottom, int32_t left) {
        if (State* p = pending()) {
            p->margin_top = top;
            p->margin_right = right;
            p->margin_bottom = bottom;
            p->margin_left = left;
        }
    });
    on_set_keyboard_interactivity([this, pending](ZwlrLayerSurfaceV1*, uint32_t interactive) {
        if (this->version() < 4)
            interactive = interactive ? 1 : 0;
        else if (interactive > 2) {
            post_error(uint32_t(Error::InvalidKeyboardInteractivity), "no such keyboard interactivity");
            return;
        }
        if (State* p = pending())
            p->keyboard_interactive = interactive;
    });
    on_set_layer([this, pending](ZwlrLayerSurfaceV1*, uint32_t layer) {
        if (layer > 3) {
            post_error(uint32_t(ZwlrLayerShellV1::Error::InvalidLayer), "no such layer");
            return;
        }
        if (State* p = pending())
            p->layer = layer;
    });
    on_set_exclusive_edge([this, pending](ZwlrLayerSurfaceV1*, uint32_t edge) {
        if (edge > (kTop | kBottom | kLeft | kRight)) {
            post_error(uint32_t(Error::InvalidExclusiveEdge), "no such edge");
            return;
        }
        if (State* p = pending())
            p->exclusive_edge = edge;
    });
    on_get_popup([this](ZwlrLayerSurfaceV1*, wl_resource* popup_resource) {
        auto* popup = dynamic_cast<Popup*>(XdgPopup::from(popup_resource));
        if (!popup || !surface_)
            return;
        if (popup->parent()) {
            post_error(uint32_t(-1), "the popup already has a parent");
            return;
        }
        popup->set_parent(surface_);
        popups_.push_back(popup);
        events.new_popup.emit(popup);
    });
    on_ack_configure([this, pending](ZwlrLayerSurfaceV1*, uint32_t serial) {
        auto it = std::ranges::find_if(sent_, [serial](const Sent& s) { return s.serial == serial; });
        if (it == sent_.end()) {
            post_error(uint32_t(Error::InvalidSurfaceState), "no configure with that serial");
            return;
        }
        const Sent acked = *it;
        sent_.erase(sent_.begin(), it + 1);
        configured_ = true;
        if (State* p = pending()) {
            p->configure_serial = acked.serial;
            p->actual_width = acked.width;
            p->actual_height = acked.height;
        }
    });
}

LayerSurface::~LayerSurface() {
    if (surface_) {
        surface_->unmap();
        surface_->clear_role(this);
    }
    reset();
    gone();
}

LayerSurface* LayerSurface::from(Surface* surface) {
    return surface ? dynamic_cast<LayerSurface*>(surface->role()) : nullptr;
}

const LayerSurface::State& LayerSurface::current() const {
    static const State none{};
    return surface_ ? surface_->current().layer : none;
}

const LayerSurface::State& LayerSurface::pending() const {
    static const State none{};
    return surface_ ? surface_->pending().layer : none;
}

void LayerSurface::gone() {
    if (gone_)
        return;
    gone_ = true;
    events.destroy.emit();
}

void LayerSurface::reset() {
    configured_ = initialized_ = false;
    for (Popup* p : std::exchange(popups_, {}))
        p->dismiss();
    sent_.clear();
}

uint32_t LayerSurface::configure(uint32_t width, uint32_t height) {
    if (closed_ || !initialized_)
        return 0;
    const uint32_t serial = wl_display_next_serial(wl_client_get_display(client()));
    sent_.push_back({serial, width, height});
    send_configure(serial, width, height);
    return serial;
}

void LayerSurface::close() {
    if (closed_)
        return;
    closed_ = true;
    send_closed();
    if (surface_)
        surface_->unmap();
    reset();
    gone();
}

bool LayerSurface::precommit(Surface& s) {
    if (closed_)
        return true;  // inert: its commits change nothing we show
    const SurfaceState& p = s.pending();
    if ((p.committed & SurfaceState::Buffer) && p.buffer && !configured_) {
        post_error(uint32_t(ZwlrLayerShellV1::Error::AlreadyConstructed), "a buffer before the first configure");
        return false;
    }
    const State& l = p.layer;
    if (l.desired_width == 0 && (l.anchor & (kLeft | kRight)) != (kLeft | kRight)) {
        post_error(uint32_t(Error::InvalidSize), "width 0 without both left and right anchors");
        return false;
    }
    if (l.desired_height == 0 && (l.anchor & (kTop | kBottom)) != (kTop | kBottom)) {
        post_error(uint32_t(Error::InvalidSize), "height 0 without both top and bottom anchors");
        return false;
    }
    if ((l.anchor & l.exclusive_edge) != l.exclusive_edge) {
        post_error(uint32_t(Error::InvalidExclusiveEdge), "the exclusive edge isn't one it is anchored to");
        return false;
    }
    return true;
}

void LayerSurface::commit(Surface& s) {
    if (closed_)
        return;
    if ((s.current().committed & SurfaceState::Buffer) && !s.buffer()) {
        s.unmap();
        reset();
        return;
    }
    const bool initial = !initialized_;
    initialized_ = true;
    if (initial)
        events.initial_commit.emit();
    if (!s.mapped() && s.buffer() && configured_)
        s.map();
}

LayerShell::LayerShell(wl_display* display) : display_(display) {
    global_ = Global::create<ZwlrLayerShellV1>(display, 5, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* shell = make<ZwlrLayerShellV1>(client, version, id);
        if (!shell)
            return;
        shell->on_get_layer_surface([this](ZwlrLayerShellV1* self, uint32_t id, wl_resource* surface_res,
                                           wl_resource* output_res, uint32_t layer, const char* name_space) {
            Surface* surface = Surface::from(surface_res);
            if (!surface)
                return;
            if (layer > 3) {
                self->post_error(uint32_t(ZwlrLayerShellV1::Error::InvalidLayer), "no such layer");
                return;
            }
            if ((surface->role_name() && surface->role_name() != LayerSurface::kRole) || surface->role()) {
                self->post_error(uint32_t(ZwlrLayerShellV1::Error::Role), "the surface has another role");
                return;
            }
            if (surface->buffer() || surface->pending().buffer) {
                self->post_error(uint32_t(ZwlrLayerShellV1::Error::AlreadyConstructed),
                                 "the surface already has a buffer");
                return;
            }
            Output* output = output_res ? Output::from(output_res) : nullptr;
            auto* ls = make<LayerSurface>(self->client(), self->version(), id, surface, output, layer,
                                          name_space ? name_space : "");
            if (!ls)
                return;
            surface->set_role(ls, nullptr, 0);
            new_surface.emit(ls);
        });
        std::erase_if(shells_, [](const auto& w) { return !w; });
        shells_.push_back(shell);
    });
}

LayerShell::~LayerShell() {
    global_.reset();
    for (auto& s : shells_)
        if (s)
            s->detach();
}

} // namespace atrium::wl
