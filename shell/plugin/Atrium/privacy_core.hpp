#pragma once
// What the privacy dots in the menu bar count, without Qt: an app recording
// from a microphone (a capture stream on a real source, as plasma-pa's
// indicator counts them), one holding a camera open, and what's sharing
// the screen.

#include <map>
#include <string>
#include <vector>

namespace atrium::privacy {

enum class Kind { Screen, Camera, Microphone };

struct Use {
    Kind kind;
    std::string app;
    bool operator==(const Use&) const = default;
};

// A sound capture stream, as libpulse describes it.
struct Capture {
    std::string app;         // application.name
    std::string media_name;  // media.name
    bool monitor = false;    // on a speaker's monitor, not a microphone
    bool corked = false;     // paused: not listening
};
// The apps listening to a microphone now: not monitors, paused streams,
// level meters (pavucontrol's "Peak detect") or atrium's own.
std::vector<std::string> microphone_apps(const std::vector<Capture>& streams);

// "/dev/video0" and the like.
bool is_camera(const std::string& path);
// Who holds a camera open: a process's command name for each pid whose open
// files (fd targets) include one. PipeWire stands for the apps it serves.
std::vector<std::string> camera_apps(const std::map<int, std::vector<std::string>>& fds,
                                     const std::map<int, std::string>& names);
std::string display_name(const std::string& comm);

// One entry per kind and app, screen first, then camera, then microphone.
std::vector<Use> merge(std::vector<Use> uses);

} // namespace atrium::privacy
