// atriumctl: talk to a running atrium over its control socket.

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

void usage() {
    std::fputs(
        "usage: atriumctl [-j] [-s SOCKET] COMMAND [ARGS]\n"
        "\n"
        "  version                   compositor version\n"
        "  windows                   open windows\n"
        "  outputs                   monitors\n"
        "  layers                    panels, docks and overlays (layer surfaces)\n"
        "  spaces                    spaces and secret spaces\n"
        "  space N                   go to space N\n"
        "  secret NAME               show or hide a secret space\n"
        "  send ID N|NAME            move a window to space N or secret space NAME\n"
        "  get [KEY]                 a setting, or all of them\n"
        "  set KEY VALUE             change a setting (VALUE is JSON, or plain text)\n"
        "  reset KEY                 back to the default\n"
        "  schema                    every setting with its type and range\n"
        "  output NAME FIELD=VALUE... change a display (width, height, refresh, scale, x, y, enabled, transform,\n"
        "                            adaptive_sync, hdr, sdr_brightness)\n"
        "  output create | output remove NAME   add or take away a virtual screen (headless, or nested)\n"
        "  devices                   mice and touchpads, with their own settings\n"
        "  device NAME FIELD=VALUE... a device's own speed, acceleration, natural_scroll, left_handed (null: shared)\n"
        "  apps                      apps the registry knows: where they open, Dock pins\n"
        "  app ID [FIELD=VALUE...]   show or change an app (secret, space, launch, maximized, fullscreen)\n"
        "  forget ID                 drop everything remembered about an app\n"
        "  dock [ID...]              the Dock's pins, or set them in this order\n"
        "  rules                     pattern rules (by title or app id pattern)\n"
        "  rule add FIELD=VALUE...   add one (app_pattern, title_pattern, secret, space, ...)\n"
        "  rule rm ID                remove one\n"
        "  shortcuts                 key combinations and what they do\n"
        "  shortcut add KEYS ACTION [ARG] | shortcut rm ID | shortcut reset\n"
        "  action NAME [ARG]         run an action (terminal, close, quit, spawn CMD, ...)\n"
        "  focus|close|minimize|maximize|fullscreen [ID]   act on a window (default: focused)\n"
        "  move ID X Y | resize ID W H\n"
        "  watch [TOPIC...]          print events (windows, settings, outputs)\n"
        "\n"
        "  -j  print raw JSON\n"
        "  -s  socket path (default: $ATRIUM_SOCKET, or the only atrium running)\n",
        stderr);
}

int connect_to(const std::string& path);

// $ATRIUM_SOCKET, else the one live atrium socket in the runtime dir. Files
// left behind by a session that died are skipped: nothing accepts on them.
std::string find_socket() {
    if (const char* s = std::getenv("ATRIUM_SOCKET"); s && *s)
        return s;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime)
        return {};
    std::vector<std::string> live;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(runtime, ec)) {
        const std::string name = e.path().filename();
        if (!name.starts_with("atrium.") || !name.ends_with(".sock"))
            continue;
        if (int fd = connect_to(e.path()); fd >= 0) {
            close(fd);
            live.push_back(e.path());
        }
    }
    if (live.size() == 1)
        return live[0];
    if (live.size() > 1)
        std::fprintf(stderr, "atriumctl: several atrium sessions are running; pick one with -s\n");
    return {};
}

int connect_to(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return -1;
    std::strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool send_line(int fd, const json& msg) {
    std::string s = msg.dump() + "\n";
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        off += size_t(n);
    }
    return true;
}

// Reads one line; false at end of stream.
bool read_line(int fd, std::string& buf, std::string& line) {
    for (;;) {
        if (auto nl = buf.find('\n'); nl != std::string::npos) {
            line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            return true;
        }
        char chunk[4096];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        buf.append(chunk, size_t(n));
    }
}

// "16" → 16, "true" → true, "#ff0000" → "#ff0000", "us" → "us".
json parse_value(const std::string& text) {
    try {
        return json::parse(text);
    } catch (const json::exception&) {
        return text;
    }
}

std::string flags(const json& w) {
    std::string f;
    for (const char* k : {"focused", "minimized", "maximized", "fullscreen", "urgent", "xwayland"})
        if (w.value(k, false))
            f += std::string(f.empty() ? "" : " ") + k;
    return f;
}

void print_human(const std::string& cmd, const json& r) {
    if (cmd == "windows") {
        for (const auto& w : r) {
            const auto& g = w["geometry"];
            std::printf("%-4llu %-24s %-14s %dx%d+%d+%d  %s  %s\n", w["id"].get<unsigned long long>(),
                        w["app_id"].get<std::string>().c_str(),
                        (w.value("secret", false) ? "secret:" + w.value("space", std::string())
                                                  : "space " + w.value("space", std::string())).c_str(),
                        g["width"].get<int>(), g["height"].get<int>(),
                        g["x"].get<int>(), g["y"].get<int>(), w["title"].get<std::string>().c_str(),
                        flags(w).c_str());
        }
    } else if (cmd == "outputs") {
        for (const auto& o : r) {
            const auto& g = o["geometry"];
            std::printf("%-10s %dx%d+%d+%d @%.2fHz scale %.2f%s%s\n", o["name"].get<std::string>().c_str(),
                        g["width"].get<int>(), g["height"].get<int>(), g["x"].get<int>(), g["y"].get<int>(),
                        o["refresh"].get<double>(), o["scale"].get<double>(),
                        o["enabled"].get<bool>() ? "" : " (off)", o["focused"].get<bool>() ? " (focused)" : "");
        }
    } else if (cmd == "layers") {
        for (const auto& l : r) {
            const auto& g = l["geometry"];
            std::printf("%-24s %-8s %-10s %dx%d+%d+%d%s\n", l["namespace"].get<std::string>().c_str(),
                        l["layer"].get<std::string>().c_str(), l["output"].get<std::string>().c_str(),
                        g["width"].get<int>(), g["height"].get<int>(), g["x"].get<int>(), g["y"].get<int>(),
                        l["mapped"].get<bool>() ? "" : " (hidden)");
        }
    } else if (cmd == "spaces") {
        for (const auto& s : r)
            std::printf("%-26s %-8s %2d window%s%s\n", s["id"].get<std::string>().c_str(),
                        s["output"].get<std::string>().c_str(), s["windows"].get<int>(),
                        s["windows"].get<int>() == 1 ? " " : "s", s["shown"].get<bool>() ? "  (shown)" : "");
    } else if (cmd == "schema") {
        for (const auto& s : r) {
            std::printf("%-36s %-8s %s\n", s["key"].get<std::string>().c_str(),
                        s["type"].get<std::string>().c_str(), s["title"].get<std::string>().c_str());
        }
    } else if (cmd == "apps" || cmd == "dock") {
        for (const auto& a : r) {
            if (cmd == "dock" && a["dock"].is_null())
                continue;
            std::string where = !a["secret"].get<std::string>().empty() ? "secret:" + a["secret"].get<std::string>()
                              : a["space"].get<int>() ? "space " + std::to_string(a["space"].get<int>()) : "";
            if (!a["launch"].get<std::string>().empty())
                where += " (launch: " + a["launch"].get<std::string>() + ")";
            std::printf("%-32s %-40s %s\n", a["app_id"].get<std::string>().c_str(), where.c_str(),
                        a["dock"].is_null() ? "" : ("dock #" + std::to_string(a["dock"].get<int>() + 1)).c_str());
        }
    } else if (cmd == "rules") {
        for (const auto& x : r)
            std::printf("%-4lld app %-24s title %-24s → %s%s\n", x["id"].get<long long>(),
                        x["app_pattern"].get<std::string>().c_str(), x["title_pattern"].get<std::string>().c_str(),
                        !x["secret"].get<std::string>().empty() ? ("secret:" + x["secret"].get<std::string>()).c_str()
                        : x["space"].get<int>() ? ("space " + std::to_string(x["space"].get<int>())).c_str() : "-",
                        x["fullscreen"] == true ? " fullscreen" : x["maximized"] == true ? " maximized" : "");
    } else if (cmd == "shortcuts") {
        for (const auto& k : r)
            std::printf("%-4lld %-30s %-18s %s%s\n", k["id"].get<long long>(), k["keys"].get<std::string>().c_str(),
                        k["action"].get<std::string>().c_str(), k.value("arg", std::string()).c_str(),
                        k.value("locked", false) ? "  (also locked)" : "");
    } else if (cmd == "get" && r.is_object()) {
        for (const auto& [k, v] : r.items())
            std::printf("%-36s %s\n", k.c_str(), v.is_string() ? v.get<std::string>().c_str() : v.dump().c_str());
    } else if (cmd == "version") {
        std::printf("atrium %s (build %d, protocol %d)\n", r["version"].get<std::string>().c_str(),
                    r["build"].get<int>(), r["protocol"].get<int>());
    } else if (!r.is_null()) {
        std::puts(r.is_string() ? r.get<std::string>().c_str() : r.dump(2).c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    bool raw = false;
    std::string socket_path;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        const std::string a = argv[i];
        if (a == "-j" || a == "--json")
            raw = true;
        else if ((a == "-s" || a == "--socket") && i + 1 < argc)
            socket_path = argv[++i];
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    if (i >= argc) {
        usage();
        return 2;
    }
    const std::string cmd = argv[i++];
    std::vector<std::string> args(argv + i, argv + argc);
    auto need = [&](size_t n) {
        if (args.size() < n) {
            usage();
            std::exit(2);
        }
    };

    json req;
    if (cmd == "version" || cmd == "windows" || cmd == "outputs" || cmd == "layers") {
        req = {{"cmd", cmd}};
    } else if (cmd == "schema") {
        req = {{"cmd", "settings.schema"}};
    } else if (cmd == "spaces") {
        req = {{"cmd", "spaces"}};
    } else if (cmd == "space") {
        need(1);
        req = {{"cmd", "space.switch"}, {"number", std::stoi(args[0])}};
    } else if (cmd == "secret") {
        need(1);
        req = {{"cmd", "secret.toggle"}, {"name", args[0]}};
    } else if (cmd == "send") {
        need(2);
        req = {{"cmd", "window.to_space"}, {"window", std::stoull(args[0])}};
        if (!args[1].empty() && std::isdigit(static_cast<unsigned char>(args[1][0])))
            req["number"] = std::stoi(args[1]);
        else
            req["secret"] = args[1];
    } else if (cmd == "get") {
        req = {{"cmd", "settings.get"}};
        if (!args.empty())
            req["key"] = args[0];
    } else if (cmd == "set") {
        need(2);
        std::string value = args[1];
        for (size_t k = 2; k < args.size(); ++k)
            value += " " + args[k];
        req = {{"cmd", "settings.set"}, {"key", args[0]}, {"value", parse_value(value)}};
    } else if (cmd == "reset") {
        need(1);
        req = {{"cmd", "settings.reset"}, {"key", args[0]}};
    } else if (cmd == "action") {
        need(1);
        req = {{"cmd", "action"}, {"name", args[0]}};
        if (args.size() > 1) {
            std::string arg = args[1];
            for (size_t k = 2; k < args.size(); ++k)
                arg += " " + args[k];
            req["arg"] = arg;
        }
    } else if (cmd == "focus" || cmd == "close" || cmd == "minimize" || cmd == "maximize" || cmd == "fullscreen") {
        req = {{"cmd", "window." + cmd}};
        if (!args.empty())
            req["window"] = std::stoull(args[0]);
    } else if (cmd == "move") {
        need(3);
        req = {{"cmd", "window.move"}, {"window", std::stoull(args[0])}, {"x", std::stoi(args[1])}, {"y", std::stoi(args[2])}};
    } else if (cmd == "resize") {
        need(3);
        req = {{"cmd", "window.resize"}, {"window", std::stoull(args[0])},
               {"width", std::stoi(args[1])}, {"height", std::stoi(args[2])}};
    } else if (cmd == "apps" || (cmd == "dock" && args.empty())) {
        req = {{"cmd", "apps.list"}};
    } else if (cmd == "dock") {
        req = {{"cmd", "dock.set"}, {"apps", args}};
    } else if (cmd == "app") {
        need(1);
        req = {{"cmd", "app.set"}, {"app_id", args[0]}};
        for (size_t k = 1; k < args.size(); ++k) {
            const size_t eq = args[k].find('=');
            if (eq == std::string::npos) {
                usage();
                return 2;
            }
            req[args[k].substr(0, eq)] = parse_value(args[k].substr(eq + 1));
        }
    } else if (cmd == "output" && !args.empty() && args[0] == "create") {
        req = {{"cmd", "output.create"}};
    } else if (cmd == "output" && !args.empty() && args[0] == "remove") {
        need(2);
        req = {{"cmd", "output.remove"}, {"output", args[1]}};
    } else if (cmd == "output") {
        need(2);
        req = {{"cmd", "output.set"}, {"output", args[0]}};
        for (size_t k = 1; k < args.size(); ++k) {
            const size_t eq = args[k].find('=');
            if (eq == std::string::npos) {
                usage();
                return 2;
            }
            req[args[k].substr(0, eq)] = parse_value(args[k].substr(eq + 1));
        }
    } else if (cmd == "devices") {
        req = {{"cmd", "devices"}};
    } else if (cmd == "device") {
        need(2);
        req = {{"cmd", "device.set"}, {"device", args[0]}};
        for (size_t k = 1; k < args.size(); ++k) {
            const size_t eq = args[k].find('=');
            if (eq == std::string::npos) {
                usage();
                return 2;
            }
            req[args[k].substr(0, eq)] = parse_value(args[k].substr(eq + 1));
        }
    } else if (cmd == "forget") {
        need(1);
        req = {{"cmd", "app.remove"}, {"app_id", args[0]}};
    } else if (cmd == "rules") {
        req = {{"cmd", "rules.list"}};
    } else if (cmd == "rule") {
        need(1);
        if (args[0] == "rm") {
            need(2);
            req = {{"cmd", "rule.remove"}, {"rule", std::stoll(args[1])}};
        } else if (args[0] == "add") {
            req = {{"cmd", "rule.add"}};
            for (size_t k = 1; k < args.size(); ++k) {
                const size_t eq = args[k].find('=');
                if (eq == std::string::npos) {
                    usage();
                    return 2;
                }
                const std::string field = args[k].substr(0, eq), value = args[k].substr(eq + 1);
                // Patterns stay text even when they look like numbers.
                req[field] = field.ends_with("_pattern") ? json(value) : parse_value(value);
            }
        } else {
            usage();
            return 2;
        }
    } else if (cmd == "shortcuts") {
        req = {{"cmd", "shortcuts.list"}};
    } else if (cmd == "shortcut") {
        need(1);
        if (args[0] == "reset") {
            req = {{"cmd", "shortcuts.reset"}};
        } else if (args[0] == "rm") {
            need(2);
            req = {{"cmd", "shortcut.remove"}, {"shortcut", std::stoll(args[1])}};
        } else if (args[0] == "add") {
            need(3);
            req = {{"cmd", "shortcut.add"}, {"keys", args[1]}, {"action", args[2]}};
            if (args.size() > 3) {
                std::string arg = args[3];
                for (size_t k = 4; k < args.size(); ++k)
                    arg += " " + args[k];
                req["arg"] = arg;
            }
        } else {
            usage();
            return 2;
        }
    } else if (cmd == "watch") {
        json topics = args.empty() ? json::array({"windows", "settings", "outputs"}) : json(args);
        req = {{"cmd", "subscribe"}, {"topics", topics}};
    } else {
        usage();
        return 2;
    }

    if (socket_path.empty())
        socket_path = find_socket();
    if (socket_path.empty()) {
        std::fprintf(stderr, "atriumctl: atrium isn't running (no socket found)\n");
        return 1;
    }
    int fd = connect_to(socket_path);
    if (fd < 0) {
        std::fprintf(stderr, "atriumctl: can't connect to %s: %s\n", socket_path.c_str(), std::strerror(errno));
        return 1;
    }

    req["id"] = 1;
    std::string buf, line;
    if (!send_line(fd, req) || !read_line(fd, buf, line)) {
        std::fprintf(stderr, "atriumctl: atrium closed the connection\n");
        return 1;
    }
    json reply = json::parse(line, nullptr, false);
    if (reply.is_discarded() || !reply.value("ok", false)) {
        std::fprintf(stderr, "atriumctl: %s\n",
                     reply.is_discarded() ? "garbled reply" : reply.value("error", "failed").c_str());
        return 1;
    }

    if (cmd == "watch") {
        while (read_line(fd, buf, line)) {
            std::cout << line << '\n' << std::flush;
        }
        return 0;
    }

    if (raw)
        std::cout << reply["result"].dump(2) << '\n';
    else
        print_human(cmd, reply["result"]);
    close(fd);
    return 0;
}
