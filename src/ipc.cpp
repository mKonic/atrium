#include "ipc.hpp"

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
    };
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

    if (cmd == "outputs") {
        json list = json::array();
        for (Output* o : server_.outputs)
            list.push_back({
                {"name", o->wlr->name},
                {"description", o->wlr->description ? o->wlr->description : ""},
                {"enabled", o->enabled()},
                {"geometry", box_json(o->box)},
                {"usable", box_json(o->usable)},
                {"scale", o->wlr->scale},
                {"refresh", o->wlr->refresh / 1000.0},
                {"focused", o == server_.focused_output},
            });
        return ok(list);
    }

    if (cmd == "spaces") {
        json list = json::array();
        for (const auto& s : server_.spaces) {
            int count = 0;
            for (View* v : server_.views)
                count += v->space == s.get();
            list.push_back({{"id", s->id()}, {"label", s->label()}, {"number", s->number}, {"secret", s->secret},
                            {"output", s->output ? s->output->wlr->name : ""}, {"shown", s->shown()},
                            {"windows", count}});
        }
        return ok(list);
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
