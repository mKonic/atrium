#include "wl/output.hpp"

#include "xdg-output-unstable-v1-server.hpp"

#include <cmath>

namespace atrium::wl {

namespace {

int32_t integer_scale(double scale) {
    return std::max(1, int32_t(std::ceil(scale)));
}

void send_xdg(ZxdgOutputV1* x, const OutputInfo& i, bool with_names) {
    x->send_logical_position(i.x, i.y);
    x->send_logical_size(i.logical_width, i.logical_height);
    if (with_names && x->version() >= 2) {
        // The name is fixed for an output's life; only send it first time.
        x->send_name(i.name.c_str());
    }
    if (x->version() >= 2)
        x->send_description(i.description.c_str());
}

} // namespace

OutputResource::OutputResource(wl_client* client, uint32_t version, uint32_t id, Output* o)
    : WlOutput(client, version, id), output(o) {}

Output::Output(wl_display* display, const OutputInfo& info) : info_(info) {
    global_ = Global::create<WlOutput>(display, 4, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* r = make<OutputResource>(client, version, id, this);
        if (!r)
            return;
        std::erase_if(resources_, [](const auto& w) { return !w; });
        resources_.push_back(r);
        send_all(r);
        if (r->version() >= 2)
            r->send_done();
    });
}

Output::~Output() {
    global_.reset();
    for (auto& r : resources_)
        if (r) {
            r->output = nullptr;
            r->detach();
        }
    for (auto& x : xdg_outputs_)
        if (x)
            x->detach();
}

void Output::send_all(OutputResource* r) const {
    const OutputInfo& i = info_;
    r->send_geometry(i.x, i.y, i.physical_width, i.physical_height, i.subpixel, i.make.c_str(), i.model.c_str(),
                     i.transform);
    r->send_mode(uint32_t(WlOutput::Mode::Current), i.mode_width, i.mode_height, i.refresh);
    if (r->version() >= 2)
        r->send_scale(integer_scale(i.scale));
    if (r->version() >= 4) {
        r->send_name(i.name.c_str());
        r->send_description(i.description.c_str());
    }
}

void Output::update(const OutputInfo& info) {
    if (info == info_)
        return;
    const OutputInfo old = std::exchange(info_, info);
    const bool geometry = old.x != info.x || old.y != info.y || old.physical_width != info.physical_width ||
                          old.physical_height != info.physical_height || old.subpixel != info.subpixel ||
                          old.make != info.make || old.model != info.model || old.transform != info.transform;
    const bool mode =
        old.mode_width != info.mode_width || old.mode_height != info.mode_height || old.refresh != info.refresh;
    const bool scale = integer_scale(old.scale) != integer_scale(info.scale);
    const bool described = old.description != info.description;
    const bool logical = old.x != info.x || old.y != info.y || old.logical_width != info.logical_width ||
                         old.logical_height != info.logical_height;
    for (auto& w : resources_) {
        OutputResource* r = w.get();
        if (!r)
            continue;
        if (geometry)
            r->send_geometry(info.x, info.y, info.physical_width, info.physical_height, info.subpixel,
                             info.make.c_str(), info.model.c_str(), info.transform);
        if (mode)
            r->send_mode(uint32_t(WlOutput::Mode::Current), info.mode_width, info.mode_height, info.refresh);
        if (scale && r->version() >= 2)
            r->send_scale(integer_scale(info.scale));
        if (described && r->version() >= 4)
            r->send_description(info.description.c_str());
    }
    if (logical || described)
        for (auto& w : xdg_outputs_)
            if (auto* x = static_cast<ZxdgOutputV1*>(w.get())) {
                send_xdg(x, info, false);
                if (x->version() < 3)
                    x->send_done();
            }
    for (auto& w : resources_)
        if (OutputResource* r = w.get(); r && r->version() >= 2)
            r->send_done();
}

std::vector<WlOutput*> Output::resources_for(wl_client* client) const {
    std::vector<WlOutput*> out;
    for (const auto& w : resources_)
        if (OutputResource* r = w.get(); r && r->client() == client)
            out.push_back(r);
    return out;
}

Output* Output::from(WlOutput* resource) {
    auto* r = dynamic_cast<OutputResource*>(resource);
    return r ? r->output : nullptr;
}

Output* Output::from(wl_resource* resource) {
    return from(WlOutput::from(resource));
}

XdgOutputs::XdgOutputs(wl_display* display) {
    global_ = Global::create<ZxdgOutputManagerV1>(display, 3, [this](wl_client* client, uint32_t version,
                                                                      uint32_t id) {
        auto* m = make<ZxdgOutputManagerV1>(client, version, id);
        if (!m)
            return;
        m->on_get_xdg_output([](ZxdgOutputManagerV1* self, uint32_t id, wl_resource* output_resource) {
            auto* x = make<ZxdgOutputV1>(self->client(), self->version(), id);
            Output* output = Output::from(output_resource);
            if (!x || !output)
                return;  // a screen already gone: the object says nothing
            std::erase_if(output->xdg_outputs_, [](const auto& w) { return !w; });
            output->xdg_outputs_.push_back(x);
            send_xdg(x, output->info(), true);
            if (x->version() < 3)
                x->send_done();
            else if (auto* r = WlOutput::from(output_resource); r && r->version() >= 2)
                r->send_done();
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
    });
}

XdgOutputs::~XdgOutputs() {
    global_.reset();
    for (auto& m : managers_)
        if (m)
            m->detach();
}

} // namespace atrium::wl
