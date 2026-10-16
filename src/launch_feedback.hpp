#pragma once
// Launch feedback, as KWin's startupfeedback effect: while an app started
// from the Dock or the launcher hasn't opened its window, its icon bounces
// beside the pointer, for at most 5 seconds. The shell asks for it with the
// activation token it hands the app (IPC activation.token with app_id and
// icon); a window opening, or the app activating with its token, ends it.

#include <wayland-server-core.h>

#include <string>
#include <vector>

struct wlr_scene_tree;
struct wlr_scene_buffer;
struct wlr_buffer;

namespace atrium {

class Server;

class LaunchFeedback {
public:
    explicit LaunchFeedback(Server& server);
    ~LaunchFeedback();
    LaunchFeedback(const LaunchFeedback&) = delete;
    LaunchFeedback& operator=(const LaunchFeedback&) = delete;

    // `icon`: a PNG of the app's icon.
    void start(const std::string& token, const std::string& app_id, const std::string& icon);
    // The startup with this token (or for this app) is over.
    void finish_token(const std::string& token);
    void finish_app(const std::string& app_id);
    // A window opened and took focus (KWin: any new active window ends it).
    void window_opened();
    bool active() const { return !startups_.empty(); }

private:
    struct Startup {
        std::string token, app_id, icon;
        long long started_ms;
    };
    void show();
    void tick();
    void stop();

    Server& server_;
    std::vector<Startup> startups_;
    std::string shown_icon_;
    wlr_scene_tree* tree_ = nullptr;
    wlr_scene_buffer* node_ = nullptr;
    wlr_buffer* buffer_ = nullptr;
    int icon_size_ = 16;
    wl_event_source* timer_ = nullptr;
    long long phase_start_ms_ = 0;
};

} // namespace atrium
