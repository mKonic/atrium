#pragma once
// Screens that exist only in memory: virtual displays (screen sharing to a
// "second monitor", tests). Frames come at the refresh rate, and whatever is
// committed counts as shown at once. After wlroots' headless backend (MIT).
#include "backend/backend.hpp"

#include <vector>

namespace atrium::backend {

class Headless final : public Backend {
public:
    explicit Headless(wl_event_loop* loop) : Backend(loop) {}
    ~Headless() override;

    bool start() override;
    uint32_t buffer_caps() const override;
    Output* create_output() override;
    // One of `width` x `height` (refresh in mHz, 0: 60 Hz).
    Output* add_output(int width, int height, int refresh = 0);
    bool is_virtual(const Output* o) const override;
    bool destroy_output(Output* o) override;

private:
    class HeadlessOutput;
    std::vector<HeadlessOutput*> outputs_;
    bool started_ = false;
};

} // namespace atrium::backend
