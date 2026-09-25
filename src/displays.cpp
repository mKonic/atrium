#include "output.hpp"
#include "registry.hpp"
#include "server.hpp"

#include <algorithm>
#include <cmath>

namespace atrium {

// Both ways an output changes (an output-management client, the Settings
// app through IPC) go through here: test or commit a whole configuration.
bool Server::commit_output_config(wlr_output_configuration_v1* config_in, bool test) {
    size_t n = 0;
    wlr_backend_output_state* states = wlr_output_configuration_v1_build_state(config_in, &n);
    if (!states)
        return false;

    wlr_output_swapchain_manager swapchains;
    wlr_output_swapchain_manager_init(&swapchains, backend);
    bool ok = wlr_output_swapchain_manager_prepare(&swapchains, states, n);
    if (ok && !test) {
        for (size_t i = 0; i < n; ++i) {
            wlr_swapchain* sc = wlr_output_swapchain_manager_get_swapchain(&swapchains, states[i].output);
            if (sc && !states[i].output->enabled)
                wlr_output_state_set_buffer(&states[i].base, wlr_swapchain_acquire(sc));
        }
        ok = wlr_backend_commit(backend, states, n);
        if (ok) {
            wlr_output_swapchain_manager_apply(&swapchains);
            wlr_output_configuration_head_v1* head;
            wl_list_for_each(head, &config_in->heads, link) {
                auto* o = static_cast<Output*>(head->state.output->data);
                o->asleep = false;
                // Re-adding at the same position would mark the output as
                // manually placed, so only move it when it actually moved.
                if (head->state.enabled &&
                    (o->box.x != head->state.x || o->box.y != head->state.y ||
                     !wlr_output_layout_get(output_layout, o->wlr)))
                    wlr_output_layout_add(output_layout, o->wlr, head->state.x, head->state.y);
            }
        }
    }
    wlr_output_swapchain_manager_finish(&swapchains);
    for (size_t i = 0; i < n; ++i)
        wlr_output_state_finish(&states[i].base);
    free(states);
    return ok;
}

void Server::apply_output_config(wlr_output_configuration_v1* config_in, bool test) {
    const bool ok = commit_output_config(config_in, test);
    if (ok)
        wlr_output_configuration_v1_send_succeeded(config_in);
    else
        wlr_output_configuration_v1_send_failed(config_in);
    wlr_output_configuration_v1_destroy(config_in);
    if (ok && !test)
        remember_displays();
    update_outputs();
}

std::string Server::display_id(const wlr_output* o) const {
    std::string id;
    for (const char* part : {o->make, o->model, o->serial})
        if (part && *part)
            id += (id.empty() ? "" : " ") + std::string(part);
    return id.empty() || !o->serial || !*o->serial ? (id.empty() ? "" : id + " ") + o->name : id;
}

void Server::remember_displays() {
    for (Output* o : outputs) {
        DisplayRecord d{.id = display_id(o->wlr), .enabled = o->wlr->enabled};
        d.adaptive_sync = o->adaptive_sync;
        d.hdr = o->hdr;
        d.sdr_brightness = o->sdr_brightness;
        d.sdr_color = o->sdr_color;
        if (o->wlr->enabled) {
            d.width = o->wlr->width;
            d.height = o->wlr->height;
            d.refresh = o->wlr->refresh;
            d.scale = o->wlr->scale;
            d.transform = int(o->wlr->transform);
            d.x = o->box.x;
            d.y = o->box.y;
        } else if (auto old = registry->display(d.id)) {
            d = *old;  // keep how it was when it comes back on
            d.enabled = false;
            d.adaptive_sync = o->adaptive_sync;
            d.hdr = o->hdr;
            d.sdr_brightness = o->sdr_brightness;
            d.sdr_color = o->sdr_color;
        }
        registry->put_display(d);
    }
}

void Server::restore_display(Output* output) {
    const auto d = registry->display(display_id(output->wlr));
    if (!d)
        return;
    output->adaptive_sync = d->adaptive_sync;
    output->hdr = d->hdr;
    output->sdr_brightness = d->sdr_brightness;
    output->sdr_color = d->sdr_color;
    wlr_output* w = output->wlr;
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, d->enabled);
    if (d->enabled) {
        if (d->width > 0 && d->height > 0) {
            // The closest mode it has; a custom one where it has none (nested).
            wlr_output_mode* best = nullptr;
            wlr_output_mode* mode;
            wl_list_for_each(mode, &w->modes, link)
                if (mode->width == d->width && mode->height == d->height &&
                    (!best || std::abs(mode->refresh - d->refresh) < std::abs(best->refresh - d->refresh)))
                    best = mode;
            if (best)
                wlr_output_state_set_mode(&state, best);
            else if (wl_list_empty(&w->modes))
                wlr_output_state_set_custom_mode(&state, d->width, d->height, d->refresh);
        }
        wlr_output_state_set_scale(&state, float(d->scale));
        wlr_output_state_set_transform(&state, wl_output_transform(d->transform));
    }
    if (!wlr_output_commit_state(w, &state))
        wlr_log(WLR_ERROR, "displays: couldn't restore %s as it was", w->name);
    wlr_output_state_finish(&state);
    if (d->enabled && d->x && d->y)
        wlr_output_layout_add(output_layout, w, *d->x, *d->y);
    if (d->enabled)
        output->apply_hdr();
    update_outputs();
}

std::optional<std::string> Server::configure_output(const nlohmann::json& req) {
    if (!req.contains("output") || !req["output"].is_string())
        return "output.set needs an \"output\" (its name)";
    Output* target = nullptr;
    for (Output* o : outputs)
        if (req["output"] == o->wlr->name)
            target = o;
    if (!target)
        return "no output called " + req["output"].get<std::string>();
    if (req.contains("adaptive_sync")) {
        const auto& v = req["adaptive_sync"];
        if (!v.is_string() || (v != "off" && v != "games" && v != "on"))
            return "adaptive_sync is off, games or on";
        target->adaptive_sync = v;
        wlr_output_schedule_frame(target->wlr);
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
    if (req.contains("hdr")) {
        if (!req["hdr"].is_boolean())
            return "hdr is true or false";
        if (req["hdr"] == true && !target->hdr_supported())
            return std::string(target->wlr->name) + " doesn't take HDR";
        target->hdr = req["hdr"];
    }
    if ((req.contains("hdr") || req.contains("sdr_brightness") || req.contains("sdr_color")) && target->enabled() &&
        !target->apply_hdr()) {
        target->hdr = false;
        target->apply_hdr();
        remember_displays();
        return std::string(target->wlr->name) + " didn't take the HDR signal";
    }

    // Brightness, HDR and adaptive sync need no new mode for the screens.
    static constexpr const char* kLayout[] = {"enabled", "width", "height", "refresh", "scale", "transform", "x", "y"};
    if (std::ranges::none_of(kLayout, [&](const char* k) { return req.contains(k); })) {
        wlr_output_schedule_frame(target->wlr);
        remember_displays();
        update_outputs();
        return std::nullopt;
    }

    wlr_output_configuration_v1* config = wlr_output_configuration_v1_create();
    for (Output* o : outputs) {
        wlr_output_configuration_head_v1* head = wlr_output_configuration_head_v1_create(config, o->wlr);
        head->state.x = o->box.x;
        head->state.y = o->box.y;
        if (o != target)
            continue;
        auto& st = head->state;
        if (req.contains("enabled") && req["enabled"].is_boolean())
            st.enabled = req["enabled"];
        if (req.contains("width") && req.contains("height")) {
            const int w = req.value("width", 0), h = req.value("height", 0), r = req.value("refresh", 0);
            wlr_output_mode* best = nullptr;
            wlr_output_mode* mode;
            wl_list_for_each(mode, &o->wlr->modes, link)
                if (mode->width == w && mode->height == h &&
                    (!best || std::abs(mode->refresh - r) < std::abs(best->refresh - r)))
                    best = mode;
            if (best) {
                st.mode = best;
            } else if (wl_list_empty(&o->wlr->modes)) {
                st.mode = nullptr;
                st.custom_mode = {w, h, r};
            } else {
                wlr_output_configuration_v1_destroy(config);
                return std::to_string(w) + "×" + std::to_string(h) + " isn't a mode " + o->wlr->name + " has";
            }
        }
        if (req.contains("scale") && req["scale"].is_number()) {
            const double scale = req["scale"];
            if (scale < 0.5 || scale > 4) {
                wlr_output_configuration_v1_destroy(config);
                return "scale goes from 0.5 to 4";
            }
            st.scale = float(scale);
        }
        if (req.contains("transform") && req["transform"].is_number_integer())
            st.transform = wl_output_transform(std::clamp(req["transform"].get<int>(), 0, 7));
        if (req.contains("x") && req["x"].is_number_integer())
            st.x = req["x"];
        if (req.contains("y") && req["y"].is_number_integer())
            st.y = req["y"];
    }
    // Never switch off the last screen.
    bool any_on = false;
    wlr_output_configuration_head_v1* head;
    wl_list_for_each(head, &config->heads, link)
        any_on = any_on || head->state.enabled;
    if (!any_on) {
        wlr_output_configuration_v1_destroy(config);
        return "at least one display stays on";
    }
    const bool ok = commit_output_config(config, false);
    wlr_output_configuration_v1_destroy(config);
    if (!ok)
        return "the display didn't accept that";
    remember_displays();
    update_outputs();
    return std::nullopt;
}

// --- virtual screens ---------------------------------------------------------------

std::optional<std::string> Server::create_output() {
    wlr_backend* nested = nullptr;
    wlr_multi_for_each_backend(backend, [](wlr_backend* b, void* data) {
        if (wlr_backend_is_wl(b))
            *static_cast<wlr_backend**>(data) = b;
    }, &nested);
    wlr_output* o = nullptr;
    if (nested) {
        o = wlr_wl_output_create(nested);
    } else {
        if (!headless_) {
            headless_ = wlr_headless_backend_create(loop);
            if (!headless_ || !wlr_multi_backend_add(backend, headless_) || !wlr_backend_start(headless_))
                return std::nullopt;
        }
        o = wlr_headless_add_output(headless_, 1920, 1080);
    }
    if (!o)
        return std::nullopt;
    return std::string(o->name);
}

std::optional<std::string> Server::remove_output(const std::string& name) {
    for (Output* o : outputs)
        if (name == o->wlr->name) {
            if (!wlr_output_is_wl(o->wlr) && !wlr_output_is_headless(o->wlr))
                return "only virtual screens can be removed; " + name + " is a real one";
            if (outputs.size() == 1)
                return "that is the only screen left";
            wlr_output_destroy(o->wlr);
            return std::nullopt;
        }
    return "no output " + name;
}

} // namespace atrium
