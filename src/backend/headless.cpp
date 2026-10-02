#include "backend/headless.hpp"

#include "wlr.hpp"

#include <algorithm>
#include <ctime>

namespace atrium::backend {

namespace {
size_t g_last_output = 0;
constexpr int kDefaultRefresh = 60000;  // mHz
} // namespace

class Headless::HeadlessOutput final : public Output {
public:
    HeadlessOutput(Headless& b, int w, int h, int refresh) : Output(b) {
        const size_t n = ++g_last_output;
        name = "HEADLESS-" + std::to_string(n);
        description = "Headless output " + std::to_string(n);
        width = w;
        height = h;
        set_refresh(refresh);
        frame_timer_ = wl_event_loop_add_timer(
            b.loop(),
            [](void* data) {
                static_cast<HeadlessOutput*>(data)->send_frame();
                return 0;
            },
            this);
    }
    ~HeadlessOutput() override {
        if (frame_timer_)
            wl_event_source_remove(frame_timer_);
        if (present_idle_)
            wl_event_source_remove(present_idle_);
    }

protected:
    bool test(const OutputState& s) override {
        constexpr uint32_t kSupported = OutputState::Buffer | OutputState::Damage | OutputState::ModeField |
                                        OutputState::Enabled | OutputState::Scale | OutputState::Transform |
                                        OutputState::RenderFormat | OutputState::Subpixel;
        if (s.committed & ~kSupported)
            return false;
        // Any size goes, but only as a custom mode: there is no mode list.
        return !(s.committed & OutputState::ModeField) || s.mode_type == OutputState::ModeType::Custom;
    }

    bool commit(const OutputState& s) override {
        if (!test(s))
            return false;
        if (s.committed & OutputState::ModeField)
            set_refresh(s.custom_mode.refresh);
        const bool on = (s.committed & OutputState::Enabled) ? s.enabled : enabled;
        if (on) {
            // Shown the moment it's committed; the present goes out once the
            // commit has returned.
            pending_seq_ = commit_seq + 1;
            if (!present_idle_)
                present_idle_ = wl_event_loop_add_idle(
                    backend.loop(),
                    [](void* data) {
                        auto* o = static_cast<HeadlessOutput*>(data);
                        o->present_idle_ = nullptr;
                        Present p;
                        p.commit_seq = o->pending_seq_;
                        p.presented = true;
                        o->send_present(p);
                    },
                    this);
            wl_event_source_timer_update(frame_timer_, frame_delay_ms_);
        }
        return true;
    }

private:
    void set_refresh(int r) {
        if (r <= 0)
            r = kDefaultRefresh;
        refresh = r;
        frame_delay_ms_ = std::max(1, 1000 * 1000 / r);
    }

    wl_event_source* frame_timer_ = nullptr;
    wl_event_source* present_idle_ = nullptr;
    int frame_delay_ms_ = 16;
    uint32_t pending_seq_ = 0;
};

Headless::~Headless() {
    for (HeadlessOutput* o : std::vector(outputs_))
        destroy_output(o);
    events.destroy.emit();
}

bool Headless::start() {
    started_ = true;
    for (HeadlessOutput* o : outputs_)
        events.new_output.emit(o);
    return true;
}

uint32_t Headless::buffer_caps() const {
    return WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;
}

Output* Headless::add_output(int width, int height, int refresh) {
    auto* o = new HeadlessOutput(*this, width, height, refresh);
    outputs_.push_back(o);
    if (started_)
        events.new_output.emit(o);
    return o;
}

Output* Headless::create_output() {
    return add_output(1920, 1080);
}

bool Headless::is_virtual(const Output* o) const {
    return std::ranges::find(outputs_, o) != outputs_.end();
}

bool Headless::destroy_output(Output* o) {
    auto it = std::ranges::find(outputs_, o);
    if (it == outputs_.end())
        return false;
    outputs_.erase(it);
    o->events.destroy.emit();
    delete o;
    return true;
}

} // namespace atrium::backend
