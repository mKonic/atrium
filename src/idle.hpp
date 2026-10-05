#pragma once
// atrium's own idle actions (hypridle's, powerdevil's): dim the screens,
// turn them off, lock, and sleep after a while without input, each a Power
// setting and all off by default. Something holding idle off (a playing
// video's idle inhibitor, systemd-inhibit --what=idle) holds them off.
#include "wlr.hpp"
#include "idle_core.hpp"

#include <chrono>
#include <vector>

namespace atrium {

class Output;
class Server;

class Idle {
public:
    explicit Idle(Server& server);
    ~Idle();
    Idle(const Idle&) = delete;
    Idle& operator=(const Idle&) = delete;

    // Input: every stage starts over, and what was done is undone.
    void activity();
    // A video started or stopped holding idle off.
    void inhibited_changed(bool inhibited);
    // The power.*_after settings changed.
    void reconfigure();

private:
    using Clock = std::chrono::steady_clock;
    void check();
    void act(IdleStage stage);
    void undo();
    void set_dim(float alpha);

    Server& server_;
    IdleTimes times_;
    Clock::time_point last_input_ = Clock::now();
    uint32_t done_ = 0;
    bool inhibited_ = false;
    wl_event_source* timer_ = nullptr;
    wl_event_source* fade_ = nullptr;
    wlr_scene_rect* dim_ = nullptr;
    float dim_alpha_ = 0;
    std::vector<Output*> slept_;  // screens it turned off
};

} // namespace atrium
