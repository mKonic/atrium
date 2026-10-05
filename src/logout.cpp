#include "logout.hpp"

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
constexpr int kGiveUpTicks = 100;  // 10 s for every app to quit

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
    wlr_log(WLR_INFO, "logout: asking %zu windows to close", ids.size());
    for (View* v : std::vector<View*>(server_.views))
        if (counts(v))
            v->close();
    timer_ = wl_event_loop_add_timer(server_.loop, [](void* d) {
        static_cast<Logout*>(d)->check();
        return 0;
    }, this);
    check();
}

Logout::~Logout() {
    if (timer_)
        wl_event_source_remove(timer_);
}

void Logout::check() {
    if (done_)
        return;
    const View* left = nullptr;
    for (const View* v : server_.views)
        if (counts(v)) {
            left = v;
            break;
        }
    if (!left) {
        finish();
        return;
    }
    if (++ticks_ >= kGiveUpTicks) {
        const char* title = left->title();
        const char* id = left->app_id();
        cancel(id && *id ? app_name(id) : title && *title ? title : "An app");
        return;
    }
    wl_event_source_timer_update(timer_, kTickMs);
}

void Logout::finish() {
    done_ = true;
    switch (then_) {
    case Then::LogOut:
        wlr_log(WLR_INFO, "logout: every app quit; logging out");
        server_.quit();
        break;
    case Then::Restart:
    case Then::ShutDown:
        wlr_log(WLR_INFO, "logout: every app quit; %s", then_ == Then::Restart ? "restarting" : "shutting down");
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

// The rest of the session stays as it is: what quit, quit.
void Logout::cancel(const std::string& holdout) {
    done_ = true;
    const char* what = then_ == Then::Restart ? "Restart" : then_ == Then::ShutDown ? "Shut down" : "Log out";
    wlr_log(WLR_INFO, "logout: %s cancelled, %s didn't quit", what, holdout.c_str());
    server_.notify(std::string(what) + " was cancelled", holdout + " didn't quit.");
    // Not from inside this object's own timer.
    wl_event_loop_add_idle(server_.loop, [](void* d) { static_cast<Server*>(d)->logout.reset(); }, &server_);
}

} // namespace atrium
