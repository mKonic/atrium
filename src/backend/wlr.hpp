#pragma once
// A wlroots backend (DRM, until atrium drives KMS itself) as one of ours: its
// outputs wrapped as backend::Outputs, their states translated both ways.
#include "backend/backend.hpp"

#include "listener.hpp"

#include <memory>
#include <vector>

struct wlr_backend;
struct wlr_output;

namespace atrium::backend {

class WlrBackend final : public Backend {
public:
    // Takes `wlr` (destroys it with itself).
    WlrBackend(wl_event_loop* loop, wlr_backend* wlr);
    ~WlrBackend() override;

    bool start() override;
    int drm_fd() const override;
    uint32_t buffer_caps() const override;
    bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) override;
    Output* create_output() override;
    bool is_virtual(const Output* o) const override;
    bool is_drm() const override;

    wlr_backend* wlr() const { return wlr_; }
    // The wrapper of a wlroots output of this backend.
    Output* output_of(wlr_output* o) const;

private:
    class WlrOutput;
    void forget(WlrOutput* o);
    wlr_backend* wlr_;
    Listener<wlr_output> new_output_;
    Listener<wlr_input_device> new_input_;
    std::vector<WlrOutput*> outputs_;
};

} // namespace atrium::backend
