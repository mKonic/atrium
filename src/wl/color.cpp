#include "wl/color.hpp"

#include "wl/output.hpp"

#include "color-management-v1-server.hpp"

#include <unistd.h>

#include <algorithm>

namespace atrium::wl {

namespace {

using CM = WpColorManagerV1;

template <class List>
void detach_all(List& list) {
    for (auto& w : list)
        if (w)
            w->detach();
}

// sRGB, as the protocol assumes for content that says nothing.
ImageDescription srgb() {
    ImageDescription d;
    d.tf_named = uint32_t(CM::TransferFunction::Gamma22);
    d.primaries_named = uint32_t(CM::Primaries::Srgb);
    return d;
}

} // namespace

// An image description a client holds, and whether it may ask what's in it
// (only descriptions atrium made: a screen's, a preference).
struct ColorManagement::DescriptionResource : WpImageDescriptionV1 {
    DescriptionResource(wl_client* client, uint32_t version, uint32_t id)
        : WpImageDescriptionV1(client, version, id) {}
    std::shared_ptr<const ImageDescription> description;
    bool with_information = false;
};

uint64_t ColorManagement::identity_of(const ImageDescription& d) {
    for (const auto& [desc, id] : identities_)
        if (desc == d)
            return id;
    const uint64_t id = identities_.size() + 1;
    identities_.push_back({d, id});
    return id;
}

void ColorManagement::make_description(wl_client* client, uint32_t version, uint32_t id,
                                       std::shared_ptr<const ImageDescription> d, bool with_information) {
    auto* r = make<DescriptionResource>(client, version, id);
    if (!r)
        return;
    r->description = d;
    r->with_information = with_information;
    r->on_get_information([](WpImageDescriptionV1* self_base, uint32_t info_id) {
        auto* self = static_cast<DescriptionResource*>(self_base);
        auto* info = make<WpImageDescriptionInfoV1>(self->client(), self->version(), info_id);
        if (!info)
            return;
        if (!self->with_information || !self->description) {
            self->post_error(uint32_t(WpImageDescriptionV1::Error::NoInformation), "no information for this one");
            return;
        }
        const ImageDescription& x = *self->description;
        if (x.primaries_named)
            info->send_primaries_named(x.primaries_named);
        if (x.primaries) {
            const auto& p = *x.primaries;
            info->send_primaries(p.rx, p.ry, p.gx, p.gy, p.bx, p.by, p.wx, p.wy);
        }
        if (x.tf_named)
            info->send_tf_named(x.tf_named);
        if (x.tf_power)
            info->send_tf_power(x.tf_power);
        if (x.luminances)
            info->send_luminances((*x.luminances)[0], (*x.luminances)[1], (*x.luminances)[2]);
        if (x.mastering_primaries) {
            const auto& p = *x.mastering_primaries;
            info->send_target_primaries(p.rx, p.ry, p.gx, p.gy, p.bx, p.by, p.wx, p.wy);
        }
        if (x.mastering_luminance)
            info->send_target_luminance((*x.mastering_luminance)[0], (*x.mastering_luminance)[1]);
        if (x.max_cll)
            info->send_target_max_cll(x.max_cll);
        if (x.max_fall)
            info->send_target_max_fall(x.max_fall);
        info->send_done();
        info->destroy();  // a one-shot object: the protocol has it go after done
    });
    r->send_ready(uint32_t(identity_of(*d)));
}

ColorManagement::ColorManagement(wl_display* display, Options options) : options_(std::move(options)) {
    global_ = Global::create<CM>(display, 1, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<CM>(client, version, id);
        if (!m)
            return;
        m->on_get_output([this](CM* self, uint32_t id, wl_resource* output_res) {
            auto* r = make<WpColorManagementOutputV1>(self->client(), self->version(), id);
            Output* o = Output::from(output_res);
            if (!r)
                return;
            if (!o) {
                r->detach();
                return;
            }
            r->on_get_image_description([this, o](WpColorManagementOutputV1* self, uint32_t id) {
                auto it = outputs_.find(o);
                make_description(self->client(), self->version(), id,
                                 std::make_shared<ImageDescription>(it == outputs_.end() ? srgb() : it->second),
                                 true);
            });
            std::erase_if(output_watches_, [](const Watch& w) { return !w.resource; });
            output_watches_.push_back({o, r});
        });
        m->on_get_surface([this](CM* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<WpColorManagementSurfaceV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s) {
                r->detach();
                return;
            }
            if (auto it = surfaces_.find(s); it != surfaces_.end() && it->second) {
                self->post_error(uint32_t(CM::Error::SurfaceExists), "the surface already has one");
                return;
            }
            surfaces_[s] = r;
            r->on_set_image_description([this, s](WpColorManagementSurfaceV1* self, WpImageDescriptionV1* desc,
                                                  uint32_t intent) {
                auto* d = static_cast<DescriptionResource*>(desc);
                if (!d || !d->description) {
                    self->post_error(uint32_t(WpColorManagementSurfaceV1::Error::ImageDescription),
                                     "the description isn't ready");
                    return;
                }
                if (!supports(options_.intents, intent)) {
                    self->post_error(uint32_t(WpColorManagementSurfaceV1::Error::RenderIntent),
                                     "that render intent isn't supported");
                    return;
                }
                s->pending_state().image_description = d->description;
                s->pending_state().render_intent = intent;
                s->pending_state().committed |= SurfaceState::ColorDescription;
            });
            r->on_unset_image_description([s](WpColorManagementSurfaceV1*) {
                s->pending_state().image_description.reset();
                s->pending_state().committed |= SurfaceState::ColorDescription;
            });
            r->on_gone([this, s, r] {
                if (auto it = surfaces_.find(s); it != surfaces_.end() && it->second.get() == r) {
                    surfaces_.erase(it);
                    s->pending_state().image_description.reset();
                    s->pending_state().committed |= SurfaceState::ColorDescription;
                }
            });
            // A surface that goes takes its entry along (the object goes inert).
            auto gone = std::make_shared<Connection>();
            *gone = s->events.destroy.connect([this, s, gone] {
                if (auto it = surfaces_.find(s); it != surfaces_.end()) {
                    if (auto* res = it->second.get())
                        res->detach();
                    surfaces_.erase(it);
                }
                preferred_.erase(s);
            });
        });
        m->on_get_surface_feedback([this](CM* self, uint32_t id, wl_resource* surface_res) {
            auto* r = make<WpColorManagementSurfaceFeedbackV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!r)
                return;
            if (!s) {
                r->detach();
                return;
            }
            auto preferred = [this, s] {
                auto it = preferred_.find(s);
                return std::make_shared<ImageDescription>(it == preferred_.end() ? srgb() : it->second);
            };
            r->on_get_preferred([this, preferred](WpColorManagementSurfaceFeedbackV1* self, uint32_t id) {
                make_description(self->client(), self->version(), id, preferred(), true);
            });
            r->on_get_preferred_parametric([this, preferred](WpColorManagementSurfaceFeedbackV1* self, uint32_t id) {
                make_description(self->client(), self->version(), id, preferred(), true);
            });
            std::erase_if(feedbacks_, [](const Feedback& f) { return !f.resource; });
            feedbacks_.push_back({s, r});
            auto gone = std::make_shared<Connection>();
            *gone = s->events.destroy.connect([this, s, gone] {
                for (auto& f : feedbacks_)
                    if (f.surface == s) {
                        if (auto* res = f.resource.get())
                            res->detach();
                        f.surface = nullptr;
                    }
            });
        });
        m->on_create_icc_creator([this](CM* self, uint32_t id) {
            auto* r = make<WpImageDescriptionCreatorIccV1>(self->client(), self->version(), id);
            if (!supports(options_.features, uint32_t(CM::Feature::IccV2V4))) {
                self->post_error(uint32_t(CM::Error::UnsupportedFeature), "ICC descriptions aren't supported");
                return;
            }
            if (r)
                r->detach();
        });
        m->on_create_windows_scrgb([this](CM* self, uint32_t id) {
            if (!supports(options_.features, uint32_t(CM::Feature::WindowsScrgb))) {
                if (auto* r = make<WpImageDescriptionV1>(self->client(), self->version(), id))
                    r->detach();
                self->post_error(uint32_t(CM::Error::UnsupportedFeature), "scRGB isn't supported");
                return;
            }
            ImageDescription d;
            d.tf_named = uint32_t(CM::TransferFunction::ExtLinear);
            d.primaries_named = uint32_t(CM::Primaries::Srgb);
            d.luminances = std::array<uint32_t, 3>{0, 10000, 80};
            make_description(self->client(), self->version(), id, std::make_shared<ImageDescription>(d), false);
        });
        m->on_create_parametric_creator([this](CM* self, uint32_t id) {
            auto* r = make<WpImageDescriptionCreatorParamsV1>(self->client(), self->version(), id);
            if (!r)
                return;
            if (!supports(options_.features, uint32_t(CM::Feature::Parametric))) {
                self->post_error(uint32_t(CM::Error::UnsupportedFeature), "parametric descriptions aren't supported");
                return;
            }
            using CR = WpImageDescriptionCreatorParamsV1;
            auto d = std::make_shared<ImageDescription>();
            auto have = std::make_shared<uint32_t>(0);  // which were set: bits as below
            enum : uint32_t { Tf = 1, Prim = 2, Lum = 4, MPrim = 8, MLum = 16, Cll = 32, Fall = 64 };
            auto once = [have](CR* self, uint32_t bit) {
                if (*have & bit) {
                    self->post_error(uint32_t(CR::Error::AlreadySet), "already set");
                    return false;
                }
                *have |= bit;
                return true;
            };
            auto feature = [this](CR* self, CM::Feature f) {
                if (supports(options_.features, uint32_t(f)))
                    return true;
                self->post_error(uint32_t(CR::Error::UnsupportedFeature), "not supported");
                return false;
            };
            r->on_set_tf_named([this, d, once](CR* self, uint32_t tf) {
                if (!supports(options_.transfer_functions, tf)) {
                    self->post_error(uint32_t(CR::Error::InvalidTf), "that transfer function isn't supported");
                    return;
                }
                if (once(self, Tf))
                    d->tf_named = tf;
            });
            r->on_set_tf_power([d, once, feature](CR* self, uint32_t eexp) {
                if (!feature(self, CM::Feature::SetTfPower))
                    return;
                if (eexp < 10000 || eexp > 100000) {
                    self->post_error(uint32_t(CR::Error::InvalidTf), "the exponent is out of range");
                    return;
                }
                if (once(self, Tf))
                    d->tf_power = eexp;
            });
            r->on_set_primaries_named([this, d, once](CR* self, uint32_t p) {
                if (!supports(options_.primaries, p)) {
                    self->post_error(uint32_t(CR::Error::InvalidPrimariesNamed), "those primaries aren't supported");
                    return;
                }
                if (once(self, Prim))
                    d->primaries_named = p;
            });
            r->on_set_primaries([d, once, feature](CR* self, int32_t rx, int32_t ry, int32_t gx, int32_t gy,
                                                   int32_t bx, int32_t by, int32_t wx, int32_t wy) {
                if (feature(self, CM::Feature::SetPrimaries) && once(self, Prim))
                    d->primaries = ImageDescription::Primaries{rx, ry, gx, gy, bx, by, wx, wy};
            });
            r->on_set_luminances([d, once, feature](CR* self, uint32_t min, uint32_t max, uint32_t ref) {
                if (!feature(self, CM::Feature::SetLuminances))
                    return;
                if (max * 10000 <= min || ref * 10000 <= min) {
                    self->post_error(uint32_t(CR::Error::InvalidLuminance), "the luminances are out of order");
                    return;
                }
                if (once(self, Lum))
                    d->luminances = std::array<uint32_t, 3>{min, max, ref};
            });
            r->on_set_mastering_display_primaries([d, once, feature](CR* self, int32_t rx, int32_t ry, int32_t gx,
                                                                    int32_t gy, int32_t bx, int32_t by, int32_t wx,
                                                                    int32_t wy) {
                if (feature(self, CM::Feature::SetMasteringDisplayPrimaries) && once(self, MPrim))
                    d->mastering_primaries = ImageDescription::Primaries{rx, ry, gx, gy, bx, by, wx, wy};
            });
            r->on_set_mastering_luminance([d, once](CR* self, uint32_t min, uint32_t max) {
                if (max * 10000 <= min) {
                    self->post_error(uint32_t(CR::Error::InvalidLuminance), "the luminances are out of order");
                    return;
                }
                if (once(self, MLum))
                    d->mastering_luminance = std::array<uint32_t, 2>{min, max};
            });
            r->on_set_max_cll([d, once](CR* self, uint32_t v) {
                if (once(self, Cll))
                    d->max_cll = v;
            });
            r->on_set_max_fall([d, once](CR* self, uint32_t v) {
                if (once(self, Fall))
                    d->max_fall = v;
            });
            r->on_create([this, d, have](CR* self, uint32_t id) {
                if ((*have & (Tf | Prim)) != (Tf | Prim)) {
                    if (auto* x = make<WpImageDescriptionV1>(self->client(), self->version(), id))
                        x->detach();
                    self->post_error(uint32_t(CR::Error::IncompleteSet), "a transfer function and primaries are needed");
                    return;
                }
                make_description(self->client(), self->version(), id, d, false);
                self->destroy();  // create consumes the creator
            });
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
        for (uint32_t i : options_.intents)
            m->send_supported_intent(i);
        for (uint32_t f : options_.features)
            m->send_supported_feature(f);
        for (uint32_t tf : options_.transfer_functions)
            m->send_supported_tf_named(tf);
        for (uint32_t p : options_.primaries)
            m->send_supported_primaries_named(p);
        m->send_done();
    });
}

ColorManagement::~ColorManagement() {
    global_.reset();
    detach_all(managers_);
    for (auto& w : output_watches_)
        if (w.resource)
            w.resource->detach();
    for (auto& f : feedbacks_)
        if (f.resource)
            f.resource->detach();
    for (auto& [s, w] : surfaces_)
        if (w)
            w->on_gone(nullptr), w->detach();
}

void ColorManagement::set_output_description(Output* output, const ImageDescription& d) {
    auto [it, fresh] = outputs_.try_emplace(output, d);
    if (!fresh && it->second == d)
        return;
    it->second = d;
    for (auto& w : output_watches_)
        if (w.output == output)
            if (auto* r = static_cast<WpColorManagementOutputV1*>(w.resource.get()); r && !r->inert())
                r->send_image_description_changed();
}

void ColorManagement::set_preferred(Surface* surface, const ImageDescription& d) {
    auto [it, fresh] = preferred_.try_emplace(surface, d);
    if (!fresh && it->second == d)
        return;
    it->second = d;
    const uint64_t id = identity_of(d);
    for (auto& f : feedbacks_)
        if (f.surface == surface)
            if (auto* r = static_cast<WpColorManagementSurfaceFeedbackV1*>(f.resource.get()); r && !r->inert())
                r->send_preferred_changed(uint32_t(id));
}

} // namespace atrium::wl
