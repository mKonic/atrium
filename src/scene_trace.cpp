#include "scene_trace.hpp"

#include "output.hpp"
#include "scene/dump.hpp"
#include "server.hpp"
#include "switcher.hpp"
#include "util/log.hpp"
#include "view.hpp"

#include <nlohmann/json.hpp>

#include <ctime>

namespace atrium {

namespace {

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

} // namespace

SceneTrace::~SceneTrace() {
    stop();
}

bool SceneTrace::start(const std::string& path, double seconds, uint64_t window) {
    stop();
    file_ = std::fopen(path.c_str(), "we");
    if (!file_)
        return false;
    path_ = path;
    window_ = window;
    frames_ = 0;
    started_ns_ = now_ns();
    until_ns_ = started_ns_ + int64_t(seconds * 1e9);
    alog(Log::Info, "trace: recording frames into %s for %.1f s", path.c_str(), seconds);
    return true;
}

void SceneTrace::stop() {
    if (!file_)
        return;
    std::fclose(file_);
    file_ = nullptr;
    alog(Log::Info, "trace: %llu frames in %s", static_cast<unsigned long long>(frames_), path_.c_str());
}

void SceneTrace::frame(Output& output) {
    if (!file_)
        return;
    const int64_t now = now_ns();
    if (now > until_ns_) {
        stop();
        return;
    }
    nlohmann::json line{{"t_ms", double(now - started_ns_) / 1e6},
                        {"frame", frames_},
                        {"output", output.screen->name},
                        {"switcher", server_.switcher && server_.switcher->shown()}};
    // What was redrawn (buffer pixels; at most 64 boxes), and whether round a blur.
    nlohmann::json damage = nlohmann::json::array();
    for (const pixman_box32_t& b : output.scene_output->last_damage()) {
        if (damage.size() >= 64)
            break;
        damage.push_back({b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1});
    }
    line["damage"] = std::move(damage);
    line["blur_padding"] = output.scene_output->last_blur_padding();
    line["two_pass"] = output.scene_output->last_two_pass();
    nlohmann::json windows = nlohmann::json::array();
    for (View* v : server_.views) {
        if (window_ && v->id != window_)
            continue;
        nlohmann::json w{{"id", v->id},
                         {"app_id", v->app_id() ? v->app_id() : ""},
                         {"geom", {v->geom.x, v->geom.y, v->geom.width, v->geom.height}},
                         {"visible", v->visible()}};
        if (window_ && v->tree)
            w["scene"] = scene::dump(v->tree);
        windows.push_back(std::move(w));
    }
    line["windows"] = std::move(windows);
    if (!window_)
        line["scene"] = scene::dump(server_.scene);
    else if (scene::Tree* t = server_.switcher ? server_.switcher->root() : nullptr)
        line["switcher_scene"] = scene::dump(t);
    const std::string text = line.dump() + "\n";
    std::fwrite(text.data(), 1, text.size(), file_);
    ++frames_;
}

} // namespace atrium
