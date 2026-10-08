#pragma once
// Logging out as macOS does: every app is asked to quit (its windows are
// closed), and only when they're all gone does the session end, or the
// computer restart or shut down. Apps still there after a few seconds (a
// "Save changes?" sheet) are listed, as Windows does: the user can answer
// them, cancel, or go ahead anyway, and doing nothing goes ahead after a
// while (the shell's dialog: IPC logout.cancel / logout.force, the
// "logout.waiting" and "logout.done" events).
//
// X11 apps that save through XSMP are asked to save first (one may ask
// "Save changes?", and Cancel there cancels it too); then the windows close.
//
// The apps open at the start go to $XDG_STATE_HOME/atrium/reopen.json for
// the shell to open again at the next login (session.reopen_windows).
#include "wlr.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace atrium {

class Server;

class Logout {
public:
    enum class Then { LogOut, Restart, ShutDown };
    Logout(Server& server, Then then);
    ~Logout();
    Logout(const Logout&) = delete;
    Logout& operator=(const Logout&) = delete;

    static Then parse(const std::string& arg);

    // The user's answer to the apps still open: called off, or anyway.
    void cancel_now();
    void force();

private:
    void close_windows();
    void check();
    void finish();
    void cancel(const std::string& holdout);
    // The apps still open, each once, by name.
    std::vector<std::string> holdouts() const;
    void tell_waiting(const std::vector<std::string>& apps);
    void tell_done();

    Server& server_;
    Then then_;
    wl_event_source* timer_ = nullptr;
    int ticks_ = 0;
    bool done_ = false;
    bool waiting_ = false;              // the holdouts are on screen
    std::vector<std::string> shown_;    // as last told
    int seconds_left_ = 0;
};

// logout_core.cpp: "org.kde.dolphin" → "Dolphin": an app id read as a name.
std::string app_name(std::string_view app_id);

// The apps worth reopening among `app_ids` (each once, in order).
std::vector<std::string> apps_to_reopen(const std::vector<std::string>& app_ids);

} // namespace atrium
