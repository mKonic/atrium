#include "output.hpp"
#include "util/log.hpp"
#include "registry.hpp"
#include "server.hpp"
#include "geometry.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

std::vector<Server::OutputChange> Server::current_outputs() const {
    std::vector<OutputChange> out;
    for (Output* o : outputs) {
        OutputChange c{o, o->screen->enabled, o->screen->current_mode, std::nullopt, o->screen->scale, o->screen->transform,
                       o->box.x, o->box.y, std::nullopt};
        if (!o->screen->current_mode)
            c.custom = std::array{o->screen->width, o->screen->height, o->screen->refresh};
        out.push_back(c);
    }
    return out;
}

// Both ways an output changes (an output-management client, the Settings
// app through IPC) go through here: test or commit a whole configuration.
bool Server::commit_output_config(const std::vector<OutputChange>& changes, bool test) {
    std::vector<std::pair<backend::Output*, backend::OutputState>> states;
    for (const OutputChange& c : changes) {
        backend::OutputState st;
        st.set_enabled(c.enabled);
        if (c.enabled) {
            if (c.mode)
                st.set_mode(c.mode);
            else if (c.custom)
                st.set_custom_mode((*c.custom)[0], (*c.custom)[1], (*c.custom)[2]);
            st.set_scale(c.scale);
            st.set_transform(c.transform);
            if (c.adaptive_sync)
                st.set_adaptive_sync_enabled(*c.adaptive_sync);
        }
        states.emplace_back(c.output->screen, std::move(st));
    }
    // All at once: a real screen that lights up gets its first frame with it.
    const bool ok = backend->commit(states, test);
    if (ok && !test) {
        for (const OutputChange& c : changes) {
            Output* o = c.output;
            o->asleep = false;
            // Re-adding at the same position would mark the output as
            // manually placed, so only move it when it actually moved.
            if (c.enabled && (o->box.x != c.x || o->box.y != c.y || !output_layout->contains(o->screen)))
                output_layout->add(o->screen, c.x, c.y);
        }
    }
    return ok;
}

void Server::publish_outputs() {
    std::vector<wl::OutputManagement::Head> heads;
    for (Output* o : outputs) {
        if (o->dying)
            continue;
        backend::Output* w = o->screen;
        wl::OutputManagement::Head h;
        h.name = w->name;
        h.description = w->description;
        h.make = w->make;
        h.model = w->model;
        h.serial = w->serial;
        h.physical_width = w->phys_width;
        h.physical_height = w->phys_height;
        int i = 0;
        for (const backend::Mode& mode : w->modes) {
            h.modes.push_back({mode.width, mode.height, mode.refresh, mode.preferred});
            if (&mode == w->current_mode)
                h.current_mode = i;
            ++i;
        }
        h.enabled = w->enabled && !o->asleep;
        h.custom_mode = {w->width, w->height, w->refresh, false};
        h.x = o->box.x;
        h.y = o->box.y;
        h.transform = int32_t(w->transform);
        h.scale = w->scale;
        h.adaptive_sync = w->adaptive_sync_status == backend::AdaptiveSync::Enabled;
        heads.push_back(std::move(h));
    }
    wl->output_management->set_heads(std::move(heads));
}

void Server::setup_outputs_protocols() {
    connections_.push_back(wl->output_management->apply.connect([this](wl::OutputManagement::Configuration& cfg) {
        std::vector<OutputChange> changes = current_outputs();
        for (const auto& hc : cfg.heads) {
            auto it = std::ranges::find_if(changes, [&](const OutputChange& c) { return hc.name == c.output->screen->name; });
            if (it == changes.end())
                continue;
            it->enabled = hc.enabled;
            if (!hc.enabled)
                continue;
            if (hc.mode) {
                it->mode = nullptr;
                it->custom.reset();
                for (const backend::Mode& m : it->output->screen->modes)
                    if (m.width == hc.mode->width && m.height == hc.mode->height && m.refresh == hc.mode->refresh)
                        it->mode = &m;
                if (!it->mode)
                    it->custom = std::array{hc.mode->width, hc.mode->height, hc.mode->refresh};
            }
            if (hc.position)
                std::tie(it->x, it->y) = *hc.position;
            if (hc.transform)
                it->transform = wl_output_transform(*hc.transform);
            if (hc.scale)
                it->scale = float(*hc.scale);
            it->adaptive_sync = hc.adaptive_sync;
        }
        const bool ok = commit_output_config(changes, cfg.test_only);
        cfg.done(ok);
        if (ok && !cfg.test_only)
            remember_displays();
        update_outputs();
    }));
    connections_.push_back(wl->output_power->request_mode.connect([this](wl::Output* g, bool on) {
        if (auto* o = g ? static_cast<Output*>(g->data) : nullptr)
            set_screen_power(o, on);
    }));
}

void Server::set_screen_power(Output* o, bool on) {
    backend::OutputState state;
    state.set_enabled(on);
    o->screen->commit_state(state);
    o->asleep = !on;
    wl->output_power->set_mode(o->global.get(), on);
    update_outputs();
}

std::string Server::display_id(const backend::Output* o) const {
    std::string id;
    for (const std::string* part : {&o->make, &o->model, &o->serial})
        if (!part->empty())
            id += (id.empty() ? "" : " ") + *part;
    return id.empty() || o->serial.empty() ? (id.empty() ? "" : id + " ") + o->name : id;
}

void Server::remember_displays() {
    for (Output* o : outputs) {
        DisplayRecord d{.id = display_id(o->screen), .enabled = o->screen->enabled};
        d.adaptive_sync = o->adaptive_sync;
        d.hdr = o->hdr;
        d.sdr_brightness = o->sdr_brightness;
        d.sdr_color = o->sdr_color;
        d.icc = o->icc;
        if (o->screen->enabled) {
            d.width = o->screen->width;
            d.height = o->screen->height;
            d.refresh = o->screen->refresh;
            d.scale = o->screen->scale;
            d.transform = int(o->screen->transform);
            d.x = o->box.x;
            d.y = o->box.y;
        } else if (auto old = registry->display(d.id)) {
            d = *old;  // keep how it was when it comes back on
            d.enabled = false;
            d.adaptive_sync = o->adaptive_sync;
            d.hdr = o->hdr;
            d.sdr_brightness = o->sdr_brightness;
            d.sdr_color = o->sdr_color;
            d.icc = o->icc;
        }
        registry->put_display(d);
    }
}

void Server::restore_display(Output* output) {
    const auto d = registry->display(display_id(output->screen));
    if (!d) {
        // Never set up here (or the greeter, which keeps nothing): a scale
        // for its density, so a HiDPI screen isn't tiny.
        backend::Output* w = output->screen;
        const backend::Mode* m = w->current_mode;
        const double scale = m ? geometry::default_scale(m->width, m->height, w->phys_width, w->phys_height,
                                                         geometry::internal_panel(w->name))
                               : 1.0;
        if (scale != 1.0 && std::abs(scale - w->scale) > 0.01) {
            backend::OutputState state;
            state.set_scale(float(scale));
            if (w->commit_state(state)) {
                alog(Log::Info, "displays: %s starts at scale %.2f", w->name.c_str(), scale);
                update_outputs();
            }
        }
        return;
    }
    output->adaptive_sync = d->adaptive_sync;
    output->hdr = d->hdr;
    output->sdr_brightness = d->sdr_brightness;
    output->sdr_color = d->sdr_color;
    output->icc = d->icc;
    output->apply_icc();
    backend::Output* w = output->screen;
    backend::OutputState state;
    state.set_enabled(d->enabled);
    if (d->enabled) {
        if (d->width > 0 && d->height > 0) {
            // The closest mode it has; a custom one where it has none (nested).
            const backend::Mode* best = nullptr;
            for (const backend::Mode& mode : w->modes)
                if (mode.width == d->width && mode.height == d->height &&
                    (!best || std::abs(mode.refresh - d->refresh) < std::abs(best->refresh - d->refresh)))
                    best = &mode;
            if (best)
                state.set_mode(best);
            else if (w->modes.empty())
                state.set_custom_mode(d->width, d->height, d->refresh);
        }
        state.set_scale(float(d->scale));
        state.set_transform(wl_output_transform(d->transform));
    }
    if (!w->commit_state(state))
        alog(Log::Error, "displays: couldn't restore %s as it was", w->name.c_str());
    if (d->enabled && d->x && d->y)
        output_layout->add(w, *d->x, *d->y);
    if (d->enabled)
        output->apply_hdr();
    update_outputs();
}

std::optional<std::string> Server::configure_output(const nlohmann::json& req) {
    if (!req.contains("output") || !req["output"].is_string())
        return "output.set needs an \"output\" (its name)";
    Output* target = nullptr;
    for (Output* o : outputs)
        if (req["output"] == o->screen->name)
            target = o;
    if (!target)
        return "no output called " + req["output"].get<std::string>();
    if (req.contains("adaptive_sync")) {
        const auto& v = req["adaptive_sync"];
        if (!v.is_string() || (v != "off" && v != "games" && v != "on"))
            return "adaptive_sync is off, games or on";
        target->adaptive_sync = v;
        target->screen->schedule_frame();
    }
    if (req.contains("sdr_brightness")) {
        const auto& v = req["sdr_brightness"];
        if (!v.is_number() || v.get<double>() < 0 || v.get<double>() > 100)
            return "sdr_brightness goes from 0 to 100";
        target->sdr_brightness = int(std::lround(v.get<double>()));
    }
    if (req.contains("sdr_color")) {
        const auto& v = req["sdr_color"];
        if (!v.is_number() || v.get<double>() < 0 || v.get<double>() > 100)
            return "sdr_color goes from 0 to 100";
        target->sdr_color = int(std::lround(v.get<double>()));
    }
    if (req.contains("icc")) {
        if (!req["icc"].is_string())
            return "icc is the path of a colour profile, or empty for none";
        const std::string before = target->icc;
        target->icc = req["icc"];
        std::string why;
        if (!target->apply_icc(&why)) {
            target->icc = before;
            target->apply_icc();
            return why;
        }
    }
    if (req.contains("hdr")) {
        if (!req["hdr"].is_boolean())
            return "hdr is true or false";
        if (req["hdr"] == true && !target->hdr_supported())
            return std::string(target->screen->name) + " doesn't take HDR";
        target->hdr = req["hdr"];
    }
    if ((req.contains("hdr") || req.contains("sdr_brightness") || req.contains("sdr_color")) && target->enabled() &&
        !target->apply_hdr()) {
        target->hdr = false;
        target->apply_hdr();
        remember_displays();
        return std::string(target->screen->name) + " didn't take the HDR signal";
    }

    // Brightness, HDR and adaptive sync need no new mode for the screens.
    static constexpr const char* kLayout[] = {"enabled", "width", "height", "refresh", "scale", "transform", "x", "y"};
    if (std::ranges::none_of(kLayout, [&](const char* k) { return req.contains(k); })) {
        target->screen->schedule_frame();
        remember_displays();
        update_outputs();
        return std::nullopt;
    }

    std::vector<OutputChange> changes = current_outputs();
    for (OutputChange& c : changes) {
        if (c.output != target)
            continue;
        Output* o = c.output;
        if (req.contains("enabled") && req["enabled"].is_boolean())
            c.enabled = req["enabled"];
        if (req.contains("width") && req.contains("height")) {
            const int w = req.value("width", 0), h = req.value("height", 0), r = req.value("refresh", 0);
            const backend::Mode* best = nullptr;
            for (const backend::Mode& mode : o->screen->modes)
                if (mode.width == w && mode.height == h &&
                    (!best || std::abs(mode.refresh - r) < std::abs(best->refresh - r)))
                    best = &mode;
            if (best) {
                c.mode = best;
                c.custom.reset();
            } else if (o->screen->modes.empty()) {
                c.mode = nullptr;
                c.custom = std::array{w, h, r};
            } else {
                return std::to_string(w) + "×" + std::to_string(h) + " isn't a mode " + o->screen->name + " has";
            }
        }
        if (req.contains("scale") && req["scale"].is_number()) {
            const double scale = req["scale"];
            if (scale < 0.5 || scale > 4)
                return "scale goes from 0.5 to 4";
            c.scale = float(scale);
        }
        if (req.contains("transform") && req["transform"].is_number_integer())
            c.transform = wl_output_transform(std::clamp(req["transform"].get<int>(), 0, 7));
        if (req.contains("x") && req["x"].is_number_integer())
            c.x = req["x"];
        if (req.contains("y") && req["y"].is_number_integer())
            c.y = req["y"];
    }
    // Never switch off the last screen.
    if (std::ranges::none_of(changes, [](const OutputChange& c) { return c.enabled; }))
        return "at least one display stays on";
    const bool ok = commit_output_config(changes, false);
    if (!ok)
        return "the display didn't accept that";
    remember_displays();
    update_outputs();
    return std::nullopt;
}

// --- virtual screens ---------------------------------------------------------------

std::optional<std::string> Server::create_output() {
    // Nested: another window on the host; else a headless screen of ours.
    backend::Output* o = backend->create_output();
    if (!o) {
        if (!headless_) {
            auto h = std::make_unique<backend::Headless>(loop);
            headless_ = h.get();
            backend->add(std::move(h));
        }
        o = headless_->add_output(1920, 1080);
    }
    if (!o)
        return std::nullopt;
    return o->name;
}

std::optional<std::string> Server::remove_output(const std::string& name) {
    for (Output* o : outputs)
        if (name == o->screen->name) {
            if (!backend->is_virtual(o->screen))
                return "only virtual screens can be removed; " + name + " is a real one";
            if (outputs.size() == 1)
                return "that is the only screen left";
            backend->destroy_output(o->screen);
            return std::nullopt;
        }
    return "no output " + name;
}

} // namespace atrium
