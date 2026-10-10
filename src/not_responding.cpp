#include "not_responding.hpp"
#include "i18n.hpp"

#include "server.hpp"
#include "shell_process.hpp"
#include "view.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <set>
#include <unistd.h>

namespace atrium {

namespace {

constexpr int kTickMs = 1500;    // Hyprland's TIMER_TIMEOUT
constexpr int kMissedPings = 5;  // misc:anr_missed_pings

} // namespace

NotResponding::NotResponding(Server& server) : server_(server) {
    timer_ = wl_event_loop_add_timer(server.loop, [](void* data) {
        static_cast<NotResponding*>(data)->tick();
        return 0;
    }, this);
    wl_event_source_timer_update(timer_, kTickMs);
}

NotResponding::~NotResponding() {
    for (auto& [client, app] : apps_)
        close_dialog(app);
    if (timer_)
        wl_event_source_remove(timer_);
}

bool NotResponding::not_responding(wl_client* client) const {
    const auto it = apps_.find(client);
    return it != apps_.end() && it->second.missed > kMissedPings;
}

void NotResponding::pong(wl_client* client) {
    const auto it = apps_.find(client);
    if (it == apps_.end())
        return;
    const bool was = it->second.missed >= kMissedPings;
    it->second.missed = 0;
    close_dialog(it->second);
    if (was)
        dim(client, false);
}

void NotResponding::tick() {
    wl_event_source_timer_update(timer_, kTickMs);
    // Each app with a mapped window (one window pinged stands for all), but
    // not our own dialogs.
    std::map<wl_client*, XdgView*> present;
    std::set<pid_t> dialogs;
    for (const auto& [c, app] : apps_)
        if (app.dialog > 0)
            dialogs.insert(app.dialog);
    for (View* v : server_.views) {
        auto* x = dynamic_cast<XdgView*>(v);
        if (!x || !x->mapped || !x->surface())
            continue;
        wl_client* client = wl_resource_get_client(x->surface()->resource);
        pid_t pid = 0;
        wl_client_get_credentials(client, &pid, nullptr, nullptr);
        if (dialogs.contains(pid))
            continue;
        present.try_emplace(client, x);
    }
    for (auto it = apps_.begin(); it != apps_.end();) {
        if (!present.contains(it->first)) {
            close_dialog(it->second);
            it = apps_.erase(it);
        } else {
            ++it;
        }
    }
    if (!server_.config.anr_dialog) {
        for (auto& [client, app] : apps_) {
            if (app.missed >= kMissedPings)
                dim(client, false);
            close_dialog(app);
            app.missed = 0;
        }
        return;
    }
    for (auto& [client, view] : present) {
        App& app = apps_[client];
        wl_client_get_credentials(client, &app.pid, nullptr, nullptr);
        if (app.missed >= kMissedPings) {
            if (app.dialog < 0 && !app.said_wait) {
                open_dialog(client, app, *view);
                dim(client, true);
            }
        } else if (app.dialog > 0) {
            close_dialog(app);
        }
        if (app.missed == 0)
            app.said_wait = false;
        app.missed++;
        wlr_xdg_surface_ping(view->toplevel->base);
    }
}

void NotResponding::open_dialog(wl_client* client, App& app, const View& view) {
    const std::string title = view.title() && *view.title() ? view.title() : "(unknown)";
    const std::string cls = view.app_id() && *view.app_id() ? view.app_id() : "(unknown)";
    const nlohmann::json question{
        {"app", view.app_id() ? view.app_id() : ""},
        {"title", tr("Application Not Responding")},
        {"body", trf("An application {} - {} is not responding.\nWhat do you want to do with it?", title, cls)},
        {"grant", tr("Terminate")},
        {"deny", tr("Wait")},
    };
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) < 0)
        return;
    if (pipe2(out, O_CLOEXEC) < 0) {
        close(in[0]);
        close(in[1]);
        return;
    }
    const std::string shell = shell_binary(), file = builtin_dir() + "/notresponding.qml";
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        setsid();
        execl(shell.c_str(), shell.c_str(), file.c_str(), nullptr);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        return;
    }
    const std::string text = question.dump();
    (void)!write(in[1], text.data(), text.size());
    close(in[1]);
    app.dialog = pid;
    app.dialog_out = out[0];
    app.reply.clear();
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    app.self = this;
    app.client = client;
    app.dialog_read = wl_event_loop_add_fd(server_.loop, out[0], WL_EVENT_READABLE | WL_EVENT_HANGUP,
        [](int, uint32_t, void* data) {
            auto* a = static_cast<App*>(data);
            a->self->dialog_output(a->client);
            return 0;
        }, &app);
}

// The dialog's answer: {"response": 0} Terminate, anything else (Wait, or
// closing it) waits.
void NotResponding::dialog_output(wl_client* client) {
    const auto it = apps_.find(client);
    if (it == apps_.end())
        return;
    App& app = it->second;
    char buf[512];
    ssize_t n;
    while ((n = read(app.dialog_out, buf, sizeof buf)) > 0)
        app.reply.append(buf, size_t(n));
    if (n < 0 && errno == EAGAIN)
        return;
    // Done: the dialog closed its end.
    const auto reply = nlohmann::json::parse(app.reply, nullptr, false);
    const bool terminate = reply.is_object() && reply.value("response", 1) == 0;
    app.dialog = -1;  // it ends by itself
    close_dialog(app);
    if (terminate && app.pid > 0) {
        wlr_log(WLR_INFO, "anr: terminating pid %d at the user's word", int(app.pid));
        kill(app.pid, SIGKILL);
    } else {
        app.said_wait = true;
    }
}

void NotResponding::close_dialog(App& app) {
    if (app.dialog_read) {
        wl_event_source_remove(app.dialog_read);
        app.dialog_read = nullptr;
    }
    if (app.dialog_out >= 0) {
        close(app.dialog_out);
        app.dialog_out = -1;
    }
    if (app.dialog > 0) {
        kill(-app.dialog, SIGTERM);
        app.dialog = -1;
    }
}

void NotResponding::dim(wl_client* client, bool on) {
    for (View* v : server_.views)
        if (v->surface() && wl_resource_get_client(v->surface()->resource) == client)
            v->set_not_responding(on);
}

} // namespace atrium
