#pragma once
// Apps that stop responding, as Hyprland's ANR does it (ANRManager.cpp):
// every 1.5 s each app with windows is pinged; after five pings without an
// answer its windows are dimmed and a dialog asks whether to terminate it or
// wait (windows.anr_dialog). An answer from the app takes both away; Wait
// holds the dialog back until the app answers again. The dialog is a window
// of its own (the portal's card, notresponding.qml), as Hyprland runs
// hyprland-dialog.
#include "wlr.hpp"

#include <map>
#include <string>
#include <sys/types.h>

namespace atrium {

class Server;
class View;

class NotResponding {
public:
    explicit NotResponding(Server& server);
    ~NotResponding();
    NotResponding(const NotResponding&) = delete;
    NotResponding& operator=(const NotResponding&) = delete;

    // The app answered a ping.
    void pong(wl_client* client);
    bool not_responding(wl_client* client) const;

private:
    struct App {
        int missed = 0;
        bool said_wait = false;
        pid_t dialog = -1;
        int dialog_out = -1;
        wl_event_source* dialog_read = nullptr;
        std::string reply;
        pid_t pid = 0;
        // For the dialog's read callback (std::map keeps the App in place).
        NotResponding* self = nullptr;
        wl_client* client = nullptr;
    };

    void tick();
    void open_dialog(wl_client* client, App& app, const View& view);
    void close_dialog(App& app);
    void dialog_output(wl_client* client);
    void dim(wl_client* client, bool on);

    Server& server_;
    wl_event_source* timer_ = nullptr;
    std::map<wl_client*, App> apps_;
};

} // namespace atrium
