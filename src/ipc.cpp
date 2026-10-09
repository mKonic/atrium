#include "ipc.hpp"
#include "appmenu.hpp"
#include "clipboard_history.hpp"
#include "lock_screen.hpp"
#include "logout.hpp"
#ifdef ATRIUM_XWAYLAND
#include "xwayland_view.hpp"
#endif
#include "input_method.hpp"
#include "keyboard_conf.hpp"
#include "night_light.hpp"
#include "layer_surface.hpp"
#include "registry.hpp"
#include "rules.hpp"
#include "seat.hpp"

#include "output.hpp"
#include "server.hpp"
#include "space.hpp"
#include "version.hpp"
#include "view.hpp"

#include <algorithm>
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
    if (!s.needs.empty())
        j["needs"] = s.needs;
    if (s.custom)
        j["custom"] = true;
    if (!s.placeholder.empty())
        j["placeholder"] = s.placeholder;
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
    auto placement_fields = [&](const json& j, auto& rec) -> std::optional<std::string> {
        std::string& secret = rec.secret;
        int& space = rec.space;
        std::string& launch = rec.launch;
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
        if (!opt_bool(j, "maximized", rec.maximized) || !opt_bool(j, "fullscreen", rec.fullscreen))
            return "maximized and fullscreen are true, false or null";
        for (auto [key, field] : window_flags(rec))
            if (!opt_bool(j, key, *field))
                return std::string(key) + " is true, false or null";
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
        if (auto err = placement_fields(req, a))
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
        if (auto err = placement_fields(req, r))
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

    // Record tables (records.hpp): "<table>.list", "<record>.add",
    // "<record>.set" / "<record>.remove" with "record" (its id or key), and
    // "<table>.order" with "records" (ids in order).
    if (const size_t dot = cmd.find('.'); dot != std::string::npos) {
        const std::string noun = cmd.substr(0, dot), verb = cmd.substr(dot + 1);
        if (const RecordTable* t = record_table(noun)) {
            const bool plural = noun == t->name;
            if (plural && verb == "list")
                return ok(reg.records(*t));
            if (plural && verb == "order") {
                if (!req.contains("records") || !req["records"].is_array() ||
                    !std::ranges::all_of(req["records"], [](const json& e) { return e.is_number_integer(); }))
                    return fail(cmd + " needs \"records\": their ids in order");
                reg.order_records(*t, req["records"].get<std::vector<int64_t>>());
                changed(t->name);
                return ok();
            }
            if (!plural && (verb == "add" || verb == "set" || verb == "remove")) {
                if (auto err = check_record_fields(*t, req))
                    return fail(*err);
                if (verb == "add") {
                    if (*t->key)
                        return fail(std::string(t->singular) + " records are set by key, not added");
                    const int64_t id = reg.add_record(*t, req);
                    changed(t->name);
                    return ok(reg.record(*t, id).value_or(json()));
                }
                if (!req.contains("record"))
                    return fail(cmd + " needs a \"record\" (its " + (*t->key ? t->key : "id") + ")");
                if (verb == "remove" ? !reg.remove_record(*t, req["record"]) : !reg.set_record(*t, req["record"], req))
                    return fail("no " + std::string(t->singular) + " " + req["record"].dump());
                changed(t->name);
                return ok(verb == "set" ? reg.record(*t, req["record"]).value_or(json()) : json());
            }
        }
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
        // Apps' own shortcuts (the portal's) aren't atrium's to reset.
        std::vector<ShortcutRecord> fresh = shortcuts_from_json(default_keybinds());
        for (const ShortcutRecord& k : reg.shortcuts())
            if (k.action == "portal")
                fresh.push_back(k);
        reg.replace_shortcuts(fresh);
        changed("shortcuts");
        return ok();
    }
    return std::nullopt;
}

json box_json(const wlr_box& b) {
    return {{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}};
}

} // namespace

namespace {

// Where the window's menus are (a com.canonical.dbusmenu object), if it said.
json menu_json(const View& v) {
    wlr_surface* s = v.surface();
    const auto* a = s && v.server.appmenus ? v.server.appmenus->for_surface(s) : nullptr;
    if (!a)
        return nullptr;
    return {{"service", a->service}, {"path", a->path}};
}

// A Wayland window's process (an X11 one's client is Xwayland).
json pid_json(const View& v) {
    wlr_surface* s = v.surface();
    if (v.kind == View::Kind::X11 || !s || !s->resource)
        return nullptr;
    pid_t pid = 0;
    wl_client_get_credentials(wl_resource_get_client(s->resource), &pid, nullptr, nullptr);
    return pid > 0 ? json(pid) : json(nullptr);
}

// An X11 window's id (what its app registers its menus under).
json x11_window_json(const View& v) {
#ifdef ATRIUM_XWAYLAND
    if (v.kind == View::Kind::X11)
        return static_cast<const XwaylandView&>(v).xsurface->window_id;
#endif
    return nullptr;
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
        {"covered", v.covered},
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
        {"identifier", v.toplevel_identifier()},
        {"title_bar", v.top()},  // the geometry's top rows that are atrium's title bar
        {"menu", menu_json(v)},
        {"x11_window", x11_window_json(v)},
        {"pid", pid_json(v)},
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
            // What libinput has now, whoever set it.
            {"applied", dev ? json{{"speed", libinput_device_config_accel_is_available(dev)
                                               ? json(libinput_device_config_accel_get_speed(dev)) : json(nullptr)},
                                   {"flat", libinput_device_config_accel_get_profile(dev) == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT},
                                   {"natural_scroll", libinput_device_config_scroll_get_natural_scroll_enabled(dev) != 0},
                                   {"left_handed", libinput_device_config_left_handed_get(dev) != 0}}
                            : json(nullptr)},
        });
    }
    return list;
}

json Ipc::keyboard_json(const Server& server) {
    // Codes from the layouts in use ("us,de"), names from the keymap.
    const Config& c = server.config;
    std::string codes = c.xkb_layout.empty() ? system_keyboard().layout : c.xkb_layout;
    json layouts = json::array();
    const std::vector<std::string> names = server.seat->layout_names();
    for (size_t i = 0; i < names.size(); ++i) {
        const size_t comma = codes.find(',');
        std::string code = codes.substr(0, comma);
        codes = comma == std::string::npos ? "" : codes.substr(comma + 1);
        layouts.push_back({{"name", names[i]}, {"code", code}});
    }
    return {{"layouts", layouts}, {"active", server.seat->layout()}};
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

    // A token for xdg-activation, as from the user's input: the shell hands
    // it to an app whose notification was clicked, so the app raises the
    // window it came from (Notifications spec 1.2, ActivationToken).
    if (cmd == "activation.token") {
        wlr_xdg_activation_token_v1* token = wlr_xdg_activation_token_v1_create(server_.activation);
        if (!token)
            return fail("couldn't make a token");
        token->seat = server_.seat->wlr;
        return ok(std::string(wlr_xdg_activation_token_v1_get_name(token)));
    }

    // Clipboard history: the entries (each one's data in its file), copying
    // one back, wl-copy's job (text, or a file's contents as a type), and
    // adding to the list without copying.
    if (cmd.starts_with("clipboard.")) {
        ClipboardHistory* h = server_.clipboard_history.get();
        if (!h)
            return fail("no clipboard history here");
        // Entries are named by "entry" ("id" belongs to the request).
        auto id = [&]() -> std::string {
            return req.contains("entry") && req["entry"].is_string() ? req["entry"] : "";
        };
        if (cmd == "clipboard.list") {
            json list = json::array();
            for (const ClipboardEntry& e : h->index().entries())
                list.push_back({{"id", e.id}, {"mime", e.mime}, {"time", e.time}, {"size", e.size},
                                {"preview", e.preview}, {"file", h->file(e).string()}});
            return ok(list);
        }
        if (cmd == "clipboard.copy")
            return h->copy(id()) ? ok() : fail("no such entry");
        if (cmd == "clipboard.delete")
            return h->remove(id()) ? ok() : fail("no such entry");
        if (cmd == "clipboard.clear") {
            h->clear();
            return ok();
        }
        if (cmd == "clipboard.set") {
            if (req.contains("text") && req["text"].is_string())
                return h->set_text(req["text"]) ? ok() : fail("too long");
            if (req.contains("path") && req["path"].is_string() && req.contains("mime") && req["mime"].is_string())
                return h->set_file(req["mime"], req["path"].get<std::string>()) ? ok() : fail("can't read it");
            return fail("clipboard.set needs \"text\", or \"mime\" and \"path\"");
        }
        // Into the list, the clipboard left alone.
        if (cmd == "clipboard.add") {
            if (req.contains("text") && req["text"].is_string())
                return h->add("text/plain;charset=utf-8", req["text"]) ? ok() : fail("empty or too long");
            if (req.contains("path") && req["path"].is_string() && req.contains("mime") && req["mime"].is_string())
                return h->add_file(req["mime"], req["path"].get<std::string>()) ? ok() : fail("can't read it");
            return fail("clipboard.add needs \"text\", or \"mime\" and \"path\"");
        }
        return fail("unknown clipboard command");
    }

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
                // Variable refresh: whether the screen can, what it's set to, and whether it's on now.
                {"adaptive_sync_supported", o->wlr->adaptive_sync_supported},
                {"adaptive_sync", o->adaptive_sync},
                {"adaptive_sync_active", o->wlr->adaptive_sync_supported &&
                                             o->wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED},
                // HDR: whether the screen takes it, what it's set to, whether it's on,
                // and SDR content's brightness (0-100) with the white it gives.
                {"hdr_supported", o->hdr_supported()},
                {"hdr", o->hdr},
                {"hdr_active", o->hdr_active()},
                {"sdr_brightness", o->sdr_brightness},
                {"sdr_color", o->sdr_color},
                {"icc", o->icc},
                {"icc_hdr", o->icc_hdr},
                {"sdr_white_nits", o->sdr_white_nits()},
                {"max_luminance", o->peak_nits()},
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

    // Type text into the focused field (the emoji picker).
    if (cmd == "text.insert") {
        if (!req.contains("text") || !req["text"].is_string() || req["text"].get<std::string>().empty())
            return fail("text.insert needs a \"text\"");
        if (server_.input_method)
            server_.input_method->insert_text(req["text"]);
        return ok();
    }

    // A typed snippet keyword replaced by its text: `delete` bytes before the
    // cursor go, `text` comes in their place (as an input method would).
    if (cmd == "text.replace") {
        if (!req.contains("text") || !req["text"].is_string() || !req.contains("delete") ||
            !req["delete"].is_number_unsigned())
            return fail("text.replace needs \"delete\" (bytes before the cursor) and \"text\"");
        if (!server_.input_method || !server_.input_method->replace_text(req["delete"], req["text"]))
            return fail("no text field takes it");
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

    if (cmd == "keyboard")
        return ok(keyboard_json(server_));
    if (cmd == "logout.cancel" || cmd == "logout.force") {
        if (!server_.logout)
            return fail("not logging out");
        if (cmd == "logout.cancel")
            server_.logout->cancel_now();
        else
            server_.logout->force();
        return ok();
    }
    if (cmd == "session")
        return ok({{"locked", server_.locked},
                   {"lock_screen", server_.lock_screen && server_.lock_screen->running()}});
    if (cmd == "dock.icons") {
        // {output, icons: [{x, y, width, height, windows: [ids]}]}
        if (!req.contains("output") || !req["output"].is_string() || !req.contains("icons") || !req["icons"].is_array())
            return fail("dock.icons needs \"output\" and \"icons\"");
        std::vector<Server::DockIcon> icons;
        for (const json& e : req["icons"]) {
            if (!e.is_object() || !e.contains("windows") || !e["windows"].is_array())
                return fail("each dock icon needs x, y, width, height and windows");
            Server::DockIcon icon{{e.value("x", 0), e.value("y", 0), e.value("width", 0), e.value("height", 0)}, {}};
            for (const json& w : e["windows"])
                if (w.is_number_unsigned())
                    icon.windows.push_back(w.get<uint64_t>());
            icons.push_back(std::move(icon));
        }
        server_.dock_icons[req["output"].get<std::string>()] = std::move(icons);
        return ok();
    }
    if (cmd == "night_light")
        return server_.night_light ? ok(server_.night_light->state()) : fail("no night light here");
    if (cmd == "night_light.set") {
        if (!req.contains("active") || !req["active"].is_boolean())
            return fail("night_light.set needs \"active\"");
        if (!server_.night_light)
            return fail("no night light here");
        server_.night_light->set_active(req["active"]);
        return ok(server_.night_light->state());
    }

    if (cmd == "keyboard.layout") {
        if (!req.contains("index") || !req["index"].is_number_unsigned())
            return fail("keyboard.layout needs an \"index\"");
        server_.seat->set_layout(req["index"].get<uint32_t>());
        return ok();
    }

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
            v->set_fullscreen(has_value ? req["value"].get<bool>() : !v->fullscreen, true);
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
        } else if (cmd == "window.frame") {
            // Where and how big at once (a window layout putting it back).
            for (const char* k : {"x", "y", "width", "height"})
                if (!req.contains(k) || !req[k].is_number_integer())
                    return fail("window.frame needs x, y, width and height");
            if (v->fullscreen)
                v->set_fullscreen(false);
            if (v->maximized)
                v->set_maximized(false, false);
            if (v->snapped)
                v->unsnap(false);
            v->request_geometry({req["x"].get<int>(), req["y"].get<int>(), req["width"].get<int>(), req["height"].get<int>()});
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
