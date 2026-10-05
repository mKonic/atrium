// atriumctl: talk to a running atrium over its control socket.

#include "records.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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
        "  scene [ID]                what's drawn, as scene nodes (a window's, or everything)\n"
        "  pointer                   where the pointer is, over what, and what locks it\n"
        "  trace [SECS [ID]] | stop  record what's drawn every frame into a file (a window's and the switcher's)\n"
        "  outputs                   monitors\n"
        "  layers                    panels, docks and overlays (layer surfaces)\n"
        "  spaces                    spaces and secret spaces\n"
        "  session                   whether the session is locked\n"
        "  space N                   go to space N\n"
        "  secret NAME               show or hide a secret space\n"
        "  send ID N|NAME            move a window to space N or secret space NAME\n"
        "  get [KEY]                 a setting, or all of them\n"
        "  set KEY VALUE             change a setting (VALUE is JSON, or plain text)\n"
        "  reset KEY                 back to the default\n"
        "  schema                    every setting with its type and range\n"
        "  output NAME FIELD=VALUE... change a display (width, height, refresh, scale, x, y, enabled, transform,\n"
        "                            adaptive_sync, hdr, sdr_brightness, sdr_color)\n"
        "  output create | output remove NAME   add or take away a virtual screen (headless, or nested)\n"
        "  devices                   mice and touchpads, with their own settings\n"
        "  device NAME FIELD=VALUE... a device's own speed, acceleration, natural_scroll, left_handed (null: shared)\n"
        "  apps                      apps the registry knows: where they open, Dock pins\n"
        "  app ID [FIELD=VALUE...]   show or change an app (secret, space, launch, maximized, fullscreen,\n"
        "                            follow, floating, keep_above, sticky, no_focus)\n"
        "  forget ID                 drop everything remembered about an app\n"
        "  dock [ID...]              the Dock's pins, or set them in this order\n"
        "  rules                     pattern rules (by title or app id pattern)\n"
        "  rule add FIELD=VALUE...   add one (app_pattern, title_pattern, secret, space, ...)\n"
        "  rule rm ID                remove one\n"
        "  records TABLE             quicklinks, snippets, commands, window_sizes or launcher_entries\n"
        "  record add TABLE FIELD=VALUE... | record set TABLE ID FIELD=VALUE... | record rm TABLE ID\n"
        "  shortcuts                 key combinations and what they do\n"
        "  shortcut add KEYS ACTION [ARG] | shortcut rm ID | shortcut reset\n"
        "  screenshot FILE [output=NAME] [window=ID] [region=X,Y,W,H] [scale=S]   a PNG of everything, a\n"
        "                            screen, a window or an area\n"
        "  screencast [output=NAME] [window=ID] [region=X,Y,W,H] [cursor=0|metadata]   a PipeWire stream of it: prints\n"
        "                            its node id and keeps it going until stopped\n"
        "  clipboard [list]          clipboard history, newest first\n"
        "  clipboard copy|delete ID | clipboard clear | clipboard set TEXT\n"
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
    } else if (cmd == "clipboard") {
        for (const auto& e : r)
            if (e.is_object() && e.contains("id"))
                std::printf("%-18s %-24s %8llu  %s\n", e["id"].get<std::string>().c_str(),
                            e["mime"].get<std::string>().c_str(), e["size"].get<unsigned long long>(),
                            e["preview"].get<std::string>().c_str());
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

int run(int argc, char** argv);

int main(int argc, char** argv) {
    // A number that isn't one ("window=abc"): said, not a crash.
    try {
        return run(argc, argv);
    } catch (const std::logic_error& e) {
        std::fprintf(stderr, "atriumctl: not a number where one goes (%s)\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "atriumctl: %s\n", e.what());
        return 1;
    }
}

int run(int argc, char** argv) {
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
    if (cmd == "version" || cmd == "windows" || cmd == "outputs" || cmd == "layers" || cmd == "session") {
        req = {{"cmd", cmd}};
    } else if (cmd == "schema") {
        req = {{"cmd", "settings.schema"}};
    } else if (cmd == "spaces") {
        req = {{"cmd", "spaces"}};
    } else if (cmd == "scene") {
        req = {{"cmd", "scene.dump"}};
        if (!args.empty())
            req["window"] = std::stoull(args[0]);
    } else if (cmd == "pointer") {
        req = {{"cmd", "pointer"}};
    } else if (cmd == "trace") {
        req = {{"cmd", args.empty() || args[0] != "stop" ? "trace.start" : "trace.stop"}};
        if (!args.empty() && args[0] != "stop")
            req["seconds"] = std::stod(args[0]);
        if (args.size() > 1)
            req["window"] = std::stoull(args[1]);
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
    } else if (cmd == "screenshot" || cmd == "screencast") {
        // What: a screen, a window or an area (everything, by default).
        size_t k = 0;
        if (cmd == "screenshot") {
            need(1);
            std::error_code ec;
            req = {{"cmd", "screenshot"}, {"path", std::filesystem::absolute(args[0], ec).string()}};
            k = 1;
        } else {
            req = {{"cmd", "screencast.start"}};
        }
        for (; k < args.size(); ++k) {
            const auto eq = args[k].find('=');
            const std::string key = args[k].substr(0, eq), value = eq == std::string::npos ? "" : args[k].substr(eq + 1);
            if (key == "output")
                req["output"] = value;
            else if (key == "window")
                req["window"] = std::stoull(value);
            else if (key == "scale" && cmd == "screenshot")
                req["scale"] = std::stod(value);
            else if (key == "cursor" && cmd == "screencast")
                req["cursor"] = value == "metadata" ? json("metadata") : json(value != "0" && value != "false");
            else if (key == "region") {
                int x = 0, y = 0, w = 0, h = 0;
                if (std::sscanf(value.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) {
                    std::fprintf(stderr, "atriumctl: region=X,Y,W,H\n");
                    return 2;
                }
                req["region"] = {{"x", x}, {"y", y}, {"width", w}, {"height", h}};
            } else {
                std::fprintf(stderr, "atriumctl: %s doesn't take %s\n", cmd.c_str(), key.c_str());
                return 2;
            }
        }
    } else if (cmd == "clipboard") {
        const std::string what = args.empty() ? "list" : args[0];
        req = {{"cmd", "clipboard." + what}};
        if (what == "copy" || what == "delete") {
            need(2);
            req["entry"] = args[1];
        } else if (what == "set") {
            need(2);
            std::string text = args[1];
            for (size_t k = 2; k < args.size(); ++k)
                text += " " + args[k];
            req["text"] = text;
        }
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
    } else if (cmd == "records") {
        need(1);
        req = {{"cmd", args[0] + ".list"}};
    } else if (cmd == "record") {
        need(2);
        const atrium::RecordTable* table = atrium::record_table(args[1]);
        if (!table) {
            std::fprintf(stderr, "atriumctl: no record table %s\n", args[1].c_str());
            return 2;
        }
        size_t first = 2;
        if (args[0] == "add") {
            req = {{"cmd", std::string(table->singular) + ".add"}};
        } else if (args[0] == "set" || args[0] == "rm") {
            need(3);
            req = {{"cmd", std::string(table->singular) + (args[0] == "rm" ? ".remove" : ".set")}};
            req["record"] = *table->key ? json(args[2]) : json(std::stoll(args[2]));
            first = 3;
        } else {
            usage();
            return 2;
        }
        for (size_t k = first; k < args.size(); ++k) {
            const size_t eq = args[k].find('=');
            if (eq == std::string::npos) {
                usage();
                return 2;
            }
            // Text columns stay text even when they look like numbers.
            const std::string field = args[k].substr(0, eq), value = args[k].substr(eq + 1);
            const auto col = std::ranges::find_if(table->columns, [&](const atrium::Column& c) { return field == c.name; });
            req[field] = col != table->columns.end() && col->type == atrium::ColumnType::Text ? json(value) : parse_value(value);
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

    if (cmd == "screencast") {
        const json& r = reply["result"];
        std::printf("%u %dx%d\n", r.value("node", 0u), r.value("width", 0), r.value("height", 0));
        std::fflush(stdout);
        // The stream lasts as long as this connection, or until its screen or
        // window goes.
        while (read_line(fd, buf, line))
            if (json::parse(line, nullptr, false).value("event", "") == "screencast.ended") {
                std::fprintf(stderr, "atriumctl: the stream ended\n");
                return 0;
            }
        return 0;
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
