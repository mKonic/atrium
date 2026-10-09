#pragma once
// The parts of screen sharing that are plain data, as xdg-desktop-portal-wlr
// has them (screencast_common.c, fps_limit.c, pipewire_screencast.c): which
// PipeWire video format each DRM format is, how damage folds together, how
// long to wait before the next frame, and the restore data an app keeps to
// share the same thing again without asking.

#include <spa/param/video/raw.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atrium::cast {

// Source types and cursor modes, as the ScreenCast portal numbers them.
enum SourceType : uint32_t { Monitor = 1, Window = 2, Virtual = 4 };
enum CursorMode : uint32_t { Hidden = 1, Embedded = 2, Metadata = 4 };
// persist_mode: 0 not at all, 1 while the app runs, 2 until revoked.
enum PersistMode : uint32_t { PersistNone = 0, PersistTransient = 1, PersistPermanent = 2 };

spa_video_format pw_from_drm(uint32_t fourcc);
uint32_t drm_from_pw(spa_video_format format);
spa_video_format strip_alpha(spa_video_format format);  // UNKNOWN when it has none
int bytes_per_pixel(uint32_t fourcc);                   // -1: not one shm can carry
// wl_shm names ARGB8888 and XRGB8888 0 and 1; every other format is its fourcc.
uint32_t drm_from_shm(uint32_t shm);
uint32_t shm_from_drm(uint32_t fourcc);

struct Rect {
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};
// The smallest rectangle holding both.
Rect merge(const Rect& a, const Rect& b);
// At most `count` rectangles for PipeWire's damage meta: the first count-1 as
// they are, the rest folded into the last.
std::vector<Rect> fit_damage(const std::vector<Rect>& damage, size_t count);

// How long to wait before capturing the next frame so as not to pass
// `max_fps`, the last having started `elapsed_ns` ago (0: go now).
uint64_t frame_delay_ns(double max_fps, int64_t elapsed_ns);

// What was shared, kept by the app as restore_data ("atrium", version 1, a{sv}).
struct Choice {
    uint32_t type = Monitor;
    std::string output;      // a screen: its connector name
    std::string app_id;      // a window: its app, and its title as a tiebreak
    std::string title;
    std::string window;      // which window that is now (not kept)
    bool cursor = true;
};
constexpr const char* kRestoreVendor = "atrium";
constexpr uint32_t kRestoreVersion = 1;

// Which window a remembered choice means now: the window of the same app
// with the same title, else that app's only window; nothing otherwise.
struct Candidate {
    std::string identifier, app_id, title;
};
std::optional<std::string> match_window(const Choice& c, const std::vector<Candidate>& windows);

} // namespace atrium::cast
