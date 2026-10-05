#pragma once
// A record of what's drawn, frame by frame (IPC trace.start, `atriumctl
// trace`): every composited frame's scene nodes as a line of JSON, for a
// glitch that lasts a frame or two (the switcher's miniatures) to be read
// back after the fact. Off unless asked for, and only for a few seconds.

#include <cstdint>
#include <cstdio>
#include <string>

namespace atrium {

class Output;
class Server;

class SceneTrace {
public:
    explicit SceneTrace(Server& server) : server_(server) {}
    ~SceneTrace();

    // Records for `seconds` into `path`; `window` narrows it to one window's
    // nodes and the switcher (0: everything). False if the file won't open.
    bool start(const std::string& path, double seconds, uint64_t window);
    void stop();
    bool active() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }
    uint64_t frames() const { return frames_; }

    // A frame `output` just committed.
    void frame(Output& output);

private:
    Server& server_;
    std::FILE* file_ = nullptr;
    std::string path_;
    int64_t started_ns_ = 0, until_ns_ = 0;
    uint64_t window_ = 0, frames_ = 0;
};

} // namespace atrium
