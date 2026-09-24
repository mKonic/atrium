#include "ipc.hpp"
#include "layer_surface.hpp"
#include "registry.hpp"
#include "rules.hpp"
#include "seat.hpp"

#include "output.hpp"
#include "server.hpp"
#include "space.hpp"
#include "version.hpp"
#include "view.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace atrium {

namespace {

constexpr size_t kMaxLine = 1 << 20;     // a request larger than this is garbage
constexpr size_t kMaxBacklog = 8 << 20;  // a client this far behind is dropped

const char* type_name(SettingType t) {
    switch (t) {
    case SettingType::Bool: return "bool";
    case SettingType::Int: return "int";
    case SettingType::Float: return "float";
    case SettingType::String: return "string";
    case SettingType::Color: return "color";
    case SettingType::Choice: return "choice";
    case SettingType::Keybinds: return "keybinds";
    case SettingType::Rules: return "rules";
    case SettingType::StringList: return "list";
    }
    return "unknown";
}

json schema_json(const SettingSchema& s) {
    json j = {
        {"key", s.key}, {"type", type_name(s.type)}, {"title", s.title},
        {"description", s.description}, {"page", s.page}, {"default", s.default_value},
    };
    if (s.type == SettingType::Int || s.type == SettingType::Float) {
        j["min"] = s.min;
        j["max"] = s.max;
    }
    if (s.type == SettingType::Choice)
        j["choices"] = s.choices;
    return j;
}

// The registry's apps, rules and shortcuts: listing and changing records.
// Nothing when `cmd` is none of these.
std::optional<json> registry_command(Server& server, const std::string& cmd, const json& req) {
    auto ok = [](json result = nullptr) { return json{{"ok", true}, {"result", std::move(result)}}; };
    auto fail = [](const std::string& why) { return json{{"ok", false}, {"error", why}}; };
    Registry& reg = *server.registry;
    auto changed = [&](const char* table) {
        server.rebuild_from_registry();
        if (server.ipc)
            server.ipc->broadcast("settings", {{"event", "registry.changed"}, {"table", table}});
    };
    auto opt_bool = [](const json& j, const char* key, std::optional<bool>& out) -> bool {
        if (!j.contains(key))
            return true;
        if (j[key].is_null())
            out.reset();
        else if (j[key].is_boolean())
            out = j[key].get<bool>();
        else
            return false;
        return true;
    };
    // Where windows go, as an app record or a rule says it; checked the same
    // way the compositor will read it.
    auto placement_fields = [&](const json& j, std::string& secret, int& space, std::string& launch,
                                std::optional<bool>& maximized, std::optional<bool>& fullscreen) -> std::optional<std::string> {
        if (j.contains("secret")) {
            if (!j["secret"].is_string())
                return "secret is the name of a secret space";
            secret = j["secret"];
        }
        if (j.contains("space")) {
            if (!j["space"].is_number_integer() || j["space"].get<int>() < 0 || j["space"].get<int>() > 99)
                return "space is a number from 1 to 99 (0: none)";
            space = j["space"];
        }
        if (j.contains("launch")) {
            if (!j["launch"].is_string())
                return "launch is the command that starts the app";
            launch = j["launch"];
        }
        if (!secret.empty() && space)
            return "an app opens in a numbered space or a secret one, not both";
        if (!launch.empty() && secret.empty())
            return "launch goes with a secret space: it starts the app when that space is shown";
        if (!opt_bool(j, "maximized", maximized) || !opt_bool(j, "fullscreen", fullscreen))
            return "maximized and fullscreen are true, false or null";
        return std::nullopt;
    };

    if (cmd == "apps.list") {
        json list = json::array();
        for (const AppRecord& a : reg.apps())
            list.push_back(app_json(a));
        return ok(list);
    }
    if (cmd == "app.set") {
        if (!req.contains("app_id") || !req["app_id"].is_string() || req["app_id"].get<std::string>().empty())
            return fail("app.set needs an \"app_id\"");
        AppRecord a = reg.app(req["app_id"]).value_or(AppRecord{.app_id = req["app_id"]});
        if (auto err = placement_fields(req, a.secret, a.space, a.launch, a.maximized, a.fullscreen))
            return fail(*err);
        // Only forgetting: where a window was is remembered, not set.
        if (req.contains("placement")) {
            if (!req["placement"].is_null())
                return fail("placement can only be forgotten (null)");
            a.placement.reset();
        }
        reg.put_app(a);
        changed("apps");
        auto now = reg.app(a.app_id);
        return ok(now ? app_json(*now) : json(nullptr));
    }
    if (cmd == "app.remove") {
        if (!req.contains("app_id") || !req["app_id"].is_string())
            return fail("app.remove needs an \"app_id\"");
        reg.remove_app(req["app_id"]);
        changed("apps");
        return ok();
    }
    if (cmd == "dock.set") {
        if (!req.contains("apps") || !req["apps"].is_array() ||
            !std::ranges::all_of(req["apps"], [](const json& e) { return e.is_string(); }))
            return fail("dock.set needs \"apps\": the pinned apps in order");
        reg.set_dock(req["apps"].get<std::vector<std::string>>());
        changed("apps");
        return ok();
    }

    if (cmd == "rules.list") {
        json list = json::array();
        for (const RuleRecord& r : reg.rules())
            list.push_back(rule_json(r));
        return ok(list);
    }
    if (cmd == "rule.add" || cmd == "rule.set") {
        RuleRecord r;
        if (cmd == "rule.set") {
            if (!req.contains("rule") || !req["rule"].is_number_integer())
                return fail("rule.set needs a \"rule\" (its id)");
            bool found = false;
            for (const RuleRecord& e : reg.rules())
                if (e.id == req[cmd.substr(0, cmd.find('.'))].get<int64_t>()) {
                    r = e;
                    found = true;
                }
            if (!found)
                return fail("no rule " + req["rule"].dump());
        }
        for (auto [key, field] : {std::pair{"app_pattern", &r.app_pattern}, std::pair{"title_pattern", &r.title_pattern}})
            if (req.contains(key)) {
                if (!req[key].is_string())
                    return fail(std::string(key) + " is a pattern");
                *field = req[key];
            }
        if (auto err = placement_fields(req, r.secret, r.space, r.launch, r.maximized, r.fullscreen))
            return fail(*err);
        json probe = json::object();
        if (!r.app_pattern.empty())
            probe["app_id"] = r.app_pattern;
        if (!r.title_pattern.empty())
            probe["title"] = r.title_pattern;
        std::vector<std::string> errors;
        parse_rules(json::array({probe}), &errors);
        if (!errors.empty())
            return fail(errors.front());
        if (cmd == "rule.add")
            r.id = reg.add_rule(r);
        else
            reg.update_rule(r);
        changed("rules");
        return ok(rule_json(r));
    }
    if (cmd == "rule.remove") {
        if (!req.contains("rule") || !req["rule"].is_number_integer())
            return fail("rule.remove needs a \"rule\" (its id)");
        if (!reg.remove_rule(req["rule"]))
            return fail("no rule " + req["rule"].dump());
        changed("rules");
        return ok();
    }

    if (cmd == "actions")
        return ok(action_names());
    if (cmd == "shortcuts.list") {
        json list = json::array();
        const std::vector<ShortcutRecord> all = reg.shortcuts();
        const auto clashes = shortcut_clashes(all, server.config.mod);
        for (const ShortcutRecord& k : all) {
            json j = shortcut_json(k);
            if (auto it = clashes.find(k.id); it != clashes.end())
                j["clashes"] = it->second;
            list.push_back(std::move(j));
        }
        return ok(list);
    }
    if (cmd == "shortcut.add" || cmd == "shortcut.set") {
        ShortcutRecord k;
        if (cmd == "shortcut.set") {
            if (!req.contains("shortcut") || !req["shortcut"].is_number_integer())
                return fail("shortcut.set needs a \"shortcut\" (its id)");
            bool found = false;
            for (const ShortcutRecord& e : reg.shortcuts())
                if (e.id == req[cmd.substr(0, cmd.find('.'))].get<int64_t>()) {
                    k = e;
                    found = true;
                }
            if (!found)
                return fail("no shortcut " + req["shortcut"].dump());
        }
        for (auto [key, field] : {std::pair{"keys", &k.keys}, std::pair{"action", &k.action}, std::pair{"arg", &k.arg}})
            if (req.contains(key)) {
                if (!req[key].is_string())
                    return fail(std::string(key) + " is text");
                *field = req[key];
            }
        if (req.contains("locked") && req["locked"].is_boolean())
            k.locked = req["locked"];
        std::vector<std::string> errors;
        resolve_keybinds(json::array({shortcut_json(k)}), server.config.mod, &errors);
        if (!errors.empty())
            return fail(errors.front());
        if (cmd == "shortcut.add")
            k.id = reg.add_shortcut(k);
        else
            reg.update_shortcut(k);
        changed("shortcuts");
        return ok(shortcut_json(k));
    }
    if (cmd == "shortcut.remove") {
        if (!req.contains("shortcut") || !req["shortcut"].is_number_integer())
            return fail("shortcut.remove needs a \"shortcut\" (its id)");
        if (!reg.remove_shortcut(req["shortcut"]))
            return fail("no shortcut " + req["shortcut"].dump());
        changed("shortcuts");
        return ok();
    }
    if (cmd == "shortcuts.reset") {
        reg.replace_shortcuts(shortcuts_from_json(default_keybinds()));
        changed("shortcuts");
        return ok();
    }
    return std::nullopt;
}

json box_json(const wlr_box& b) {
    return {{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}};
}

} // namespace

json Ipc::window_json(const View& v) {
    return {
        {"id", v.id},
        {"app_id", v.app_id()},
        {"title", v.title()},
        {"geometry", box_json(v.geom)},
        {"output", v.output ? v.output->wlr->name : ""},
        {"focused", v.server.focused_view == &v},
        {"minimized", v.minimized},
        {"maximized", v.maximized},
        {"fullscreen", v.fullscreen},
        {"urgent", v.urgent},
        {"xwayland", v.kind == View::Kind::X11},
        {"space", v.space ? v.space->label() : ""},
        {"secret", v.space && v.space->secret},
        {"icon", v.icon},
        {"tag", v.tag},
        {"content_type", content_type_name(v.server, v)},
        {"modal", v.modal()},
        {"skip_taskbar", v.hidden_from_lists()},
        {"keep_above", v.keep_above},
        {"sticky", v.sticky},
        {"tiled", v.tiled()},
    };
}

json Ipc::devices_json(const Server& server) {
    json list = json::array();
    std::vector<std::string> seen;
    auto opt = [](const auto& v) { return v ? json(*v) : json(nullptr); };
    for (wlr_pointer* p : server.seat->pointer_devices()) {
        const std::string name = p->base.name ? p->base.name : "";
        // One entry per device: a mouse can show up as several nodes.
        if (name.empty() || std::ranges::find(seen, name) != seen.end())
            continue;
        seen.push_back(name);
        libinput_device* dev = wlr_input_device_is_libinput(&p->base) ? wlr_libinput_get_device_handle(&p->base) : nullptr;
        const auto own = server.registry->device(name);
        list.push_back({
            {"name", name},
            {"touchpad", dev && libinput_device_config_tap_get_finger_count(dev) > 0},
            // What the device lets be changed (a nested session's pointer: nothing).
            {"can", {{"speed", dev && libinput_device_config_accel_is_available(dev)},
                     {"natural_scroll", dev && libinput_device_config_scroll_has_natural_scroll(dev)},
                     {"left_handed", dev && libinput_device_config_left_handed_is_available(dev)}}},
            {"speed", opt(own ? own->speed : std::nullopt)},
            {"acceleration", opt(own ? own->acceleration : std::nullopt)},
            {"natural_scroll", opt(own ? own->natural_scroll : std::nullopt)},
            {"left_handed", opt(own ? own->left_handed : std::nullopt)},
        });
    }
    return list;
}

json Ipc::spaces_json(const Server& server) {
    json list = json::array();
    for (const auto& s : server.spaces) {
        int count = 0;
        for (View* v : server.views)
            count += v->space == s.get();
        list.push_back({{"id", s->id()}, {"label", s->label()}, {"number", s->number}, {"secret", s->secret},
                        {"output", s->output ? s->output->wlr->name : ""}, {"shown", s->shown()},
                        {"tiled", s->tiled},
                        {"windows", count}});
    }
    return list;
}

Ipc::Ipc(Server& server, const std::string& wayland_display) : server_(server) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return;
    path_ = std::string(runtime) + "/atrium." + wayland_display + ".sock";

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof addr.sun_path) {
        wlr_log(WLR_ERROR, "ipc: socket path too long: %s", path_.c_str());
        return;
    }
    std::strcpy(addr.sun_path, path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0)
        return;
    unlink(path_.c_str());  // a stale socket from a crashed session
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(listen_fd_, 16) < 0) {
        wlr_log_errno(WLR_ERROR, "ipc: can't listen on %s", path_.c_str());
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    listen_source_ = wl_event_loop_add_fd(server_.loop, listen_fd_, WL_EVENT_READABLE, on_accept, this);
    setenv("ATRIUM_SOCKET", path_.c_str(), 1);
    wlr_log(WLR_INFO, "ipc: listening on %s", path_.c_str());
}

Ipc::~Ipc() {
    for (Client& c : clients_)
        drop(c);
    reap();
    if (listen_source_)
        wl_event_source_remove(listen_source_);
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        unlink(path_.c_str());
    }
}

int Ipc::on_accept(int, uint32_t, void* data) {
    static_cast<Ipc*>(data)->accept_client();
    return 0;
}

void Ipc::accept_client() {
    for (;;) {
        int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        Client& c = clients_.emplace_back();
        c.owner = this;
        c.fd = fd;
        c.source = wl_event_loop_add_fd(server_.loop, fd, WL_EVENT_READABLE, on_client, &c);
    }
}

int Ipc::on_client(int, uint32_t mask, void* data) {
    auto* c = static_cast<Client*>(data);
    Ipc& ipc = *c->owner;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
        ipc.drop(*c);
    if (!c->dead && (mask & WL_EVENT_WRITABLE))
        ipc.flush(*c);
    if (!c->dead && (mask & WL_EVENT_READABLE))
        ipc.read_client(*c);
    ipc.reap();
    return 0;
}

bool Ipc::read_client(Client& c) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(c.fd, buf, sizeof buf);
        if (n > 0) {
            c.in.append(buf, size_t(n));
            if (c.in.size() > kMaxLine && c.in.find('\n') == std::string::npos) {
                drop(c);
                return false;
            }
            continue;
        }
        if (n == 0) {
            drop(c);
            return false;
        }
        if (errno == EAGAIN || errno == EINTR)
            break;
        drop(c);
        return false;
    }

    size_t nl;
    while ((nl = c.in.find('\n')) != std::string::npos) {
        std::string line = c.in.substr(0, nl);
        c.in.erase(0, nl + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        json reply;
        try {
            json req = json::parse(line);
            reply = handle(c, req);
            if (req.contains("id"))
                reply["id"] = req["id"];
        } catch (const json::exception& e) {
            reply = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
        }
        send(c, reply);
        if (c.dead)
            return false;
    }
    return true;
}

void Ipc::send(Client& c, const json& msg) {
    if (c.dead)
        return;
    c.out += msg.dump();
    c.out += '\n';
    if (c.out.size() > kMaxBacklog) {
        wlr_log(WLR_INFO, "ipc: dropping a client that stopped reading");
        drop(c);
        return;
    }
    flush(c);
}

bool Ipc::flush(Client& c) {
    while (!c.out.empty()) {
        ssize_t n = write(c.fd, c.out.data(), c.out.size());
        if (n > 0) {
            c.out.erase(0, size_t(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        drop(c);
        return false;
    }
    wl_event_source_fd_update(c.source, WL_EVENT_READABLE | (c.out.empty() ? 0 : WL_EVENT_WRITABLE));
    return true;
}

void Ipc::drop(Client& c) {
    if (c.dead)
        return;
    if (c.source)
        wl_event_source_remove(c.source);
    if (c.fd >= 0)
        close(c.fd);
    c.source = nullptr;
    c.fd = -1;
    c.dead = true;
}

void Ipc::reap() {
    std::erase_if(clients_, [](const Client& c) { return c.dead; });
}

void Ipc::broadcast(const std::string& topic, const json& event) {
    // Copy the targets first: a send can drop a client and mutate the list.
    std::vector<Client*> targets;
    for (Client& c : clients_)
        if (c.topics.contains(topic))
            targets.push_back(&c);
    for (Client* c : targets)
        send(*c, event);
    reap();
}

// --- requests ------------------------------------------------------------------------

json Ipc::handle(Client& c, const json& req) {
    auto ok = [](json result = nullptr) { return json{{"ok", true}, {"result", std::move(result)}}; };
    auto fail = [](const std::string& why) { return json{{"ok", false}, {"error", why}}; };

    if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string())
        return fail("request needs a \"cmd\"");
    const std::string cmd = req["cmd"];

    // Windows are named by "window"; "id" belongs to the request itself.
    auto find_view = [&]() -> View* {
        if (!req.contains("window") || !req["window"].is_number_integer())
            return server_.focused_view;
        const auto id = req["window"].get<uint64_t>();
        for (View* v : server_.views)
            if (v->id == id)
                return v;
        return nullptr;
    };

    if (cmd == "version")
        return ok({{"version", ATRIUM_VERSION}, {"build", ATRIUM_BUILD}, {"protocol", kIpcProtocol}});

    if (cmd == "windows") {
        json list = json::array();
        for (View* v : server_.views)
            list.push_back(window_json(*v));
        return ok(list);
    }

    if (cmd == "layers") {
        static constexpr const char* kNames[] = {"background", "bottom", "top", "overlay"};
        json list = json::array();
        for (Output* o : server_.outputs)
            for (int i = 0; i < 4; ++i)
                for (LayerSurface* l : o->layers[i]) {
                    const wlr_box g = {l->tree ? l->tree->node.x : 0, l->tree ? l->tree->node.y : 0,
                                       int(l->wlr->current.actual_width), int(l->wlr->current.actual_height)};
                    list.push_back({{"namespace", l->wlr->namespace_ ? l->wlr->namespace_ : ""},
                                    {"layer", kNames[i]}, {"output", o->wlr->name},
                                    {"geometry", box_json(g)}, {"mapped", l->mapped}});
                }
        return ok(list);
    }

    if (cmd == "outputs") {
        json list = json::array();
        for (Output* o : server_.outputs) {
            json modes = json::array();
            wlr_output_mode* mode;
            wl_list_for_each(mode, &o->wlr->modes, link)
                modes.push_back({{"width", mode->width}, {"height", mode->height}, {"refresh", mode->refresh},
                                 {"preferred", mode->preferred}});
            list.push_back({
                {"name", o->wlr->name},
                {"description", o->wlr->description ? o->wlr->description : ""},
                {"make", o->wlr->make ? o->wlr->make : ""},
                {"model", o->wlr->model ? o->wlr->model : ""},
                {"enabled", o->enabled()},
                {"geometry", box_json(o->box)},
                {"usable", box_json(o->usable)},
                {"mode", {{"width", o->wlr->width}, {"height", o->wlr->height}, {"refresh", o->wlr->refresh}}},
                {"modes", modes},
                {"scale", o->wlr->scale},
                {"transform", int(o->wlr->transform)},
                {"refresh", o->wlr->refresh / 1000.0},
                {"focused", o == server_.focused_output},
            });
        }
        return ok(list);
    }

    if (cmd == "output.create") {
        if (auto name = server_.create_output())
            return ok(json{{"output", *name}});
        return fail("couldn't create an output");
    }
    if (cmd == "output.remove") {
        if (!req.contains("output") || !req["output"].is_string())
            return fail("output.remove needs an \"output\" (its name)");
        if (auto err = server_.remove_output(req["output"]))
            return fail(*err);
        return ok();
    }

    if (cmd == "output.set") {
        if (auto err = server_.configure_output(req))
            return fail(*err);
        return ok();
    }

    // Pointing devices and their own settings (null: the shared one applies).
    if (cmd == "devices")
        return ok(devices_json(server_));

    if (cmd == "device.set") {
        if (!req.contains("device") || !req["device"].is_string())
            return fail("device.set needs a \"device\" (its name, as devices lists it)");
        DeviceRecord d = server_.registry->device(req["device"]).value_or(DeviceRecord{req["device"]});
        if (req.contains("speed")) {
            if (req["speed"].is_null())
                d.speed.reset();
            else if (req["speed"].is_number() && req["speed"] >= -1.0 && req["speed"] <= 1.0)
                d.speed = req["speed"].get<double>();
            else
                return fail("speed goes from -1 to 1 (or null: the shared one)");
        }
        if (req.contains("acceleration")) {
            if (req["acceleration"].is_null())
                d.acceleration.reset();
            else if (req["acceleration"] == "adaptive" || req["acceleration"] == "flat")
                d.acceleration = req["acceleration"].get<std::string>();
            else
                return fail("acceleration is \"adaptive\" or \"flat\" (or null)");
        }
        for (auto [key, field] : {std::pair{"natural_scroll", &DeviceRecord::natural_scroll},
                                  std::pair{"left_handed", &DeviceRecord::left_handed}}) {
            if (!req.contains(key))
                continue;
            if (req[key].is_null())
                (d.*field).reset();
            else if (req[key].is_boolean())
                d.*field = req[key].get<bool>();
            else
                return fail(std::string(key) + " is true or false (or null)");
        }
        server_.registry->put_device(d);
        server_.seat->apply_pointer_config();
        return ok(devices_json(server_));
    }

    if (cmd == "spaces")
        return ok(spaces_json(server_));

    if (cmd == "space.switch") {
        if (!req.contains("number") || !req["number"].is_number_integer())
            return fail("space.switch needs a \"number\"");
        const int n = req["number"];
        if (n < 1 || n > 99)
            return fail("space numbers go from 1 to 99");
        server_.switch_space(server_.focused_output, n);
        return ok();
    }

    if (cmd == "secret.toggle") {
        if (!req.contains("name") || !req["name"].is_string() || req["name"].get<std::string>().empty())
            return fail("secret.toggle needs a \"name\"");
        server_.toggle_secret(req["name"]);
        return ok();
    }

    if (cmd == "subscribe") {
        if (!req.contains("topics") || !req["topics"].is_array())
            return fail("subscribe needs \"topics\": [\"windows\", \"settings\", \"outputs\"]");
        for (const auto& t : req["topics"])
            if (t.is_string())
                c.topics.insert(t.get<std::string>());
        return ok();
    }

    if (auto reply = registry_command(server_, cmd, req))
        return *reply;

    if (cmd == "settings.schema") {
        json list = json::array();
        for (const auto& s : server_.settings->schema())
            list.push_back(schema_json(s));
        return ok(list);
    }

    if (cmd == "settings.get") {
        if (!req.contains("key"))
            return ok(server_.settings->all());
        const std::string key = req["key"];
        if (!server_.settings->find(key))
            return fail("no setting called " + key);
        return ok(server_.settings->get(key));
    }

    if (cmd == "settings.set" || cmd == "settings.reset") {
        if (!req.contains("key") || !req["key"].is_string())
            return fail(cmd + " needs a \"key\"");
        const std::string key = req["key"];
        std::optional<std::string> err;
        if (cmd == "settings.set") {
            if (!req.contains("value"))
                return fail("settings.set needs a \"value\"");
            err = server_.settings->set(key, req["value"]);
        } else {
            err = server_.settings->reset(key);
        }
        if (err)
            return fail(*err);
        server_.setting_changed(key);
        return ok(server_.settings->get(key));
    }

    if (cmd == "action") {
        if (!req.contains("name") || !req["name"].is_string())
            return fail("action needs a \"name\"");
        auto action = action_from_name(req["name"]);
        if (!action)
            return fail("unknown action " + req["name"].get<std::string>());
        Keybind k{0, 0, *action};
        if (req.contains("arg") && req["arg"].is_string())
            k.arg = req["arg"];
        k.iarg = std::atoi(k.arg.c_str());  // space numbers, VT numbers
        server_.run_action(k);
        return ok();
    }

    if (cmd.starts_with("window.")) {
        View* v = find_view();
        if (!v)
            return fail("no such window");
        const bool has_value = req.contains("value") && req["value"].is_boolean();
        if (cmd == "window.focus") {
            if (v->minimized)
                v->set_minimized(false);
            server_.focus_view(v);
        } else if (cmd == "window.close") {
            v->close();
        } else if (cmd == "window.minimize") {
            v->set_minimized(has_value ? req["value"].get<bool>() : true);
        } else if (cmd == "window.maximize") {
            v->set_maximized(has_value ? req["value"].get<bool>() : !v->maximized);
        } else if (cmd == "window.fullscreen") {
            v->set_fullscreen(has_value ? req["value"].get<bool>() : !v->fullscreen);
        } else if (cmd == "window.pin") {
            // On every space (sticky).
            v->sticky = has_value ? req["value"].get<bool>() : !v->sticky;
            server_.notify_window(*v, "changed");
        } else if (cmd == "window.move") {
            if (!req.contains("x") || !req.contains("y"))
                return fail("window.move needs x and y");
            v->move_to(req["x"].get<int>(), req["y"].get<int>());
        } else if (cmd == "window.to_space") {
            if (req.contains("secret") && req["secret"].is_string() && !req["secret"].get<std::string>().empty())
                server_.move_to_space(v, server_.ensure_secret(req["secret"]));
            else if (req.contains("number") && req["number"].is_number_integer() && req["number"] >= 1 &&
                     req["number"] <= 99 && server_.focused_output)
                server_.move_to_space(v, server_.ensure_space(server_.focused_output, req["number"]));
            else
                return fail("window.to_space needs a \"number\" (1-99) or a \"secret\" name");
        } else if (cmd == "window.resize") {
            if (!req.contains("width") || !req.contains("height"))
                return fail("window.resize needs width and height");
            v->request_geometry({v->geom.x, v->geom.y, req["width"].get<int>(), req["height"].get<int>()});
        } else {
            return fail("unknown command " + cmd);
        }
        return ok(window_json(*v));
    }

    return fail("unknown command " + cmd);
}

} // namespace atrium
