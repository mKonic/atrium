#include "logout.hpp"

#include "xsmp.hpp"

#include "ipc.hpp"
#include "logind.hpp"
#include "server.hpp"
#include "view.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace atrium {

namespace {

constexpr int kTickMs = 100;
constexpr int kWaitTicks = 50;      // 5 s for every app to quit before they're listed
constexpr int kAnywaySeconds = 30;  // listed, then it goes ahead by itself

bool counts(const View* v) {
    return v->mapped && !v->unmanaged() && !v->passive();
}

std::filesystem::path state_dir() {
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    if (state && *state)
        return std::filesystem::path(state) / "atrium";
    return home ? std::filesystem::path(home) / ".local/state/atrium" : std::filesystem::path();
}

} // namespace

Logout::Logout(Server& server, Then then) : server_(server), then_(then) {
    std::vector<std::string> ids;
    for (View* v : server_.views)
        if (counts(v) && !v->parent())
            ids.emplace_back(v->app_id() ? v->app_id() : "");
    if (const std::filesystem::path dir = state_dir(); !dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto file = dir / "reopen.json";
        if (server_.config.reopen_windows)
            std::ofstream(file) << nlohmann::json{{"apps", apps_to_reopen(ids)}}.dump() << "\n";
        else
            std::filesystem::remove(file, ec);
    }
    timer_ = wl_event_loop_add_timer(server_.loop, [](void* d) {
        static_cast<Logout*>(d)->check();
        return 0;
    }, this);
    // X11 apps that save through the session manager first, then every window.
    if (server_.xsmp && server_.xsmp->clients() > 0) {
        wlr_log(WLR_INFO, "logout: asking %zu X11 apps to save", server_.xsmp->clients());
        server_.xsmp->save_all([this](const std::string& holdout) {
            if (holdout.empty())
                close_windows();
            else
                cancel(holdout);
        });
        return;
    }
    close_windows();
}

void Logout::close_windows() {
    if (done_)
        return;
    size_t n = 0;
    for (View* v : std::vector<View*>(server_.views))
        if (counts(v)) {
            v->close();
            ++n;
        }
    wlr_log(WLR_INFO, "logout: asking %zu windows to close", n);
    check();
}

Logout::~Logout() {
    if (timer_)
        wl_event_source_remove(timer_);
}

std::vector<std::string> Logout::holdouts() const {
    std::vector<std::string> names;
    for (const View* v : server_.views) {
        if (!counts(v))
            continue;
        const char* id = v->app_id();
        const char* title = v->title();
        std::string name = id && *id ? app_name(id) : title && *title ? title : "An app";
        if (std::ranges::find(names, name) == names.end())
            names.push_back(std::move(name));
    }
    return names;
}

void Logout::check() {
    if (done_)
        return;
    const std::vector<std::string> left = holdouts();
    if (left.empty()) {
        finish();
        return;
    }
    ++ticks_;
    if (ticks_ == kWaitTicks) {
        waiting_ = true;
        seconds_left_ = kAnywaySeconds;
        wlr_log(WLR_INFO, "logout: still open: %zu; going ahead in %d s unless cancelled", left.size(),
                seconds_left_);
    }
    if (waiting_) {
        // A second gone: count down; on the last, anyway.
        if ((ticks_ - kWaitTicks) % (1000 / kTickMs) == 0 && ticks_ > kWaitTicks && --seconds_left_ <= 0) {
            force();
            return;
        }
        if (left != shown_ || (ticks_ - kWaitTicks) % (1000 / kTickMs) == 0)
            tell_waiting(left);
    }
    wl_event_source_timer_update(timer_, kTickMs);
}

void Logout::tell_waiting(const std::vector<std::string>& apps) {
    shown_ = apps;
    if (!server_.ipc)
        return;
    const char* then = then_ == Then::Restart ? "restart" : then_ == Then::ShutDown ? "shutdown" : "logout";
    server_.ipc->broadcast("shell", {{"event", "logout.waiting"}, {"then", then}, {"apps", apps},
                                     {"seconds", seconds_left_}});
}

void Logout::tell_done() {
    if (waiting_ && server_.ipc)
        server_.ipc->broadcast("shell", {{"event", "logout.done"}});
    waiting_ = false;
}

void Logout::force() {
    if (done_)
        return;
    wlr_log(WLR_INFO, "logout: going ahead without %zu apps", holdouts().size());
    finish();
}

// The user changed their mind: the session stays as it is (what quit, quit).
void Logout::cancel_now() {
    if (done_)
        return;
    done_ = true;
    tell_done();
    if (server_.xsmp)
        server_.xsmp->cancel();
    wlr_log(WLR_INFO, "logout: cancelled");
    wl_event_loop_add_idle(server_.loop, [](void* d) { static_cast<Server*>(d)->logout.reset(); }, &server_);
}

void Logout::finish() {
    done_ = true;
    tell_done();
    if (server_.xsmp)
        server_.xsmp->die();
    switch (then_) {
    case Then::LogOut:
        wlr_log(WLR_INFO, "logout: logging out");
        server_.quit();
        break;
    case Then::Restart:
    case Then::ShutDown:
        wlr_log(WLR_INFO, "logout: %s", then_ == Then::Restart ? "restarting" : "shutting down");
        // logind ends the session with the rest; nested (no logind), we just quit.
        if (server_.logind && server_.logind->available()) {
            if (then_ == Then::Restart)
                server_.logind->reboot();
            else
                server_.logind->power_off();
            // Refused (polkit), the session goes on, without its apps.
            wl_event_loop_add_idle(server_.loop, [](void* d) { static_cast<Server*>(d)->logout.reset(); }, &server_);
        } else {
            server_.quit();
        }
        break;
    }
}

// An app's own "Save changes?" (XSMP) answered Cancel: that's the user
// calling it off. The rest of the session stays as it is.
void Logout::cancel(const std::string& holdout) {
    done_ = true;
    tell_done();
    if (server_.xsmp)
        server_.xsmp->cancel();
    const char* what = then_ == Then::Restart ? "Restart" : then_ == Then::ShutDown ? "Shut down" : "Log out";
    wlr_log(WLR_INFO, "logout: %s cancelled, %s didn't quit", what, holdout.c_str());
    server_.notify(std::string(what) + " was cancelled", holdout + " didn't quit.");
    // Not from inside this object's own timer.
    wl_event_loop_add_idle(server_.loop, [](void* d) { static_cast<Server*>(d)->logout.reset(); }, &server_);
}

} // namespace atrium
