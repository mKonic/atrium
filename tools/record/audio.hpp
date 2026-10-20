#pragma once
// Sound, as gpu-screen-recorder's audio_capture.c and sound.c take it: a
// capture stream per device (PipeWire here, pipewire-pulse there), every
// chunk placed on the recording clock by when it was heard (the device's
// latency taken off), gaps left as silence, and the devices of one track
// summed (gsr's amix without normalizing).

#include "record_core.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct pw_thread_loop;
struct pw_context;
struct pw_core;

namespace atrium::record {

constexpr int kSampleRate = 48000, kChannels = 2;

// The recording's clock: monotonic time since it started, without the time
// spent paused (gsr's recording_clock).
class Clock {
public:
    void start(int64_t now_ns);
    void set_paused(bool paused, int64_t now_ns);
    bool paused() const;
    // Recording time at `mono_ns`; -1 before the start or while paused.
    int64_t at(int64_t mono_ns) const;

private:
    mutable std::mutex lock_;
    int64_t start_ = -1, paused_total_ = 0, paused_at_ = -1;
};

struct AudioDevice {
    std::string name, description;
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();
    // The devices there are (sources, and sinks' monitors as NAME.monitor)
    // and the defaults; false when PipeWire isn't reachable.
    bool devices(std::vector<AudioDevice>& out, std::string& default_output, std::string& default_input);
    // Connects every track's devices, reading `frame_size` samples a chunk.
    // `exit` is gsr's 50 when a device doesn't exist.
    bool start(const std::vector<AudioTrack>& tracks, int frame_size, const Clock& clock, int& exit, std::string& error);
    // A track's title for the file ("Devices: Default output, ...").
    std::string title(size_t track) const;
    // The next `n` samples (interleaved stereo floats) of a track once its
    // devices are all past them, or have fallen `late_ns` behind the clock;
    // `pos` is where they start, in samples on the recording clock.
    bool pop(size_t track, int n, int64_t now_rec_ns, int64_t late_ns, float* out, int64_t& pos);
    void stop();

    struct Source;
    struct Track;

private:
    bool connect();

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::vector<std::unique_ptr<Source>> sources_;
};

} // namespace atrium::record
