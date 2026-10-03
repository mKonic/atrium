#pragma once
// Screen casting, as KWin does it: atrium makes the PipeWire video streams
// itself (a screen, a window or part of the desktop) and atrium-portal's
// ScreenCast hands their node ids to the app. Frames are the copies the
// capture protocols make (capture.cpp), into PipeWire's buffers: DMA-BUFs
// the GPU fills when the app takes them, else memory.
#include "wl/capture.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct pw_loop;
struct pw_context;
struct pw_core;
struct wl_event_source;

namespace atrium {

class Server;

class ScreenCast {
public:
    explicit ScreenCast(Server& server);
    ~ScreenCast();
    ScreenCast(const ScreenCast&) = delete;
    ScreenCast& operator=(const ScreenCast&) = delete;

    struct Ready {
        uint32_t node = 0;  // PipeWire's node id
        int width = 0, height = 0;
        std::string error;  // why there is none
    };
    // A stream of `target` (its `cursor` drawn in or not; `cursor_metadata`:
    // sent beside the picture instead, as apps like OBS want it); `ready`
    // once PipeWire knows it, `ended` when it stops by itself (its screen or
    // window went, PipeWire did). Its id, for stop. `owner` tags it (an IPC
    // client) for stop_owned.
    uint64_t start(const wl::Capture::Target& target, bool cursor_metadata, uint64_t owner,
                   std::function<void(const Ready&)> ready, std::function<void()> ended = {});
    bool stop(uint64_t id);
    void stop_owned(uint64_t owner);
    size_t streams() const { return streams_.size(); }

    struct Stream;

private:
    bool connect();
    void disconnect();
    void reap();

    Server& server_;
    pw_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    wl_event_source* source_ = nullptr;
    wl_event_source* reap_ = nullptr;
    struct CoreListener;
    std::unique_ptr<CoreListener> core_listener_;
    std::vector<std::unique_ptr<Stream>> streams_;
    uint64_t next_id_ = 1;
    bool drop_core_ = false;
    wl::Connection stopped_;  // PipeWire went: connect anew next time
};

} // namespace atrium
