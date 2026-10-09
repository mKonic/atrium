#pragma once
// xdg-system-bell: an app rings the bell (a terminal on a Tab with nothing
// to complete), as KWin's systembell does it: the sound theme's "bell"
// through libcanberra, at most every 100 ms (windows.system_bell), and the
// window that rang, if it isn't the focused one, marked as wanting
// attention.
#include "listener.hpp"
#include "wlr.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace atrium {

class Server;

class SystemBell {
public:
    explicit SystemBell(Server& server);
    ~SystemBell();
    SystemBell(const SystemBell&) = delete;
    SystemBell& operator=(const SystemBell&) = delete;

    // The bell's sound alone (typing aids' beeps), at most every 100 ms.
    void beep() { play(); }

private:
    void ring(wlr_xdg_system_bell_v1_ring_event* e);
    void play();

    Server& server_;
    wlr_xdg_system_bell_v1* bell_;
    Listener<wlr_xdg_system_bell_v1_ring_event> ring_;
    int64_t last_sound_ms_ = 0;
    // The sound is played on a thread of its own: libcanberra blocks while
    // the audio stack doesn't answer (PipeWire down), which must never stop
    // the compositor.
    // Shared with it, so it may outlive this (detached, if it's stuck).
    struct Sound {
        std::mutex mutex;
        std::condition_variable wake;
        bool pending = false, quit = false;
    };
    std::shared_ptr<Sound> sound_;
};

} // namespace atrium
