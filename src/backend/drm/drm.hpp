#pragma once
// Real screens: a GPU's KMS device, driven with atomic commits. Each
// connected connector is an Output; a CRTC is taken for it while it is on,
// its frames go to the CRTC's primary plane and the pointer to its cursor
// plane. After wlroots' backend/drm (MIT), with aquamarine's and KWin's
// as further reference.
#include "backend/backend.hpp"
#include "backend/drm/props.hpp"
#include "backend/session.hpp"

extern "C" {
#include <wlr/render/drm_format_set.h>
}

#include <xf86drmMode.h>

#include <memory>
#include <unordered_map>
#include <vector>

struct wlr_buffer;
struct wlr_drm_syncobj_timeline;

namespace atrium::backend::drm {

class Drm final : public Backend {
public:
    // The KMS device at `path`, opened through the session; null if it has
    // no display outputs or can't be driven.
    static std::unique_ptr<Drm> create(wl_event_loop* loop, Session& session, const std::string& path);
    ~Drm() override;

    bool start() override;
    int drm_fd() const override { return fd_; }
    uint32_t buffer_caps() const override;
    bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) override;
    bool is_drm() const override { return true; }
    bool supports_timelines() const override { return timeline_; }

    const std::string& name() const { return name_; }

private:
    struct Plane;
    struct Crtc;
    struct Connector;
    struct ConnState;
    struct PageFlip;
    class ConnOutput;
    friend class ConnOutput;

    Drm(wl_event_loop* loop, Session& session);
    bool check_features();
    bool init_resources();
    void scan_connectors(uint32_t only_connector = 0);
    bool connect(Connector& c, const drmModeConnector& info);
    void disconnect(Connector& c);
    void realloc_crtcs(Connector* want);
    bool alloc_crtc(Connector& c);

    // A KMS framebuffer for `buffer` (cached with it); 0 if KMS won't take it.
    uint32_t fb_for(wlr_buffer* buffer, const wlr_drm_format_set* formats);

    bool commit_connector(Connector& c, const OutputState& state, bool test_only);
    bool commit_states(std::vector<ConnState>& states, bool modeset, bool nonblock, bool test_only, bool async);
    bool prepare(ConnState& st, bool modeset);
    void handle_page_flip(unsigned seq, unsigned sec, unsigned usec, unsigned crtc_id, PageFlip* flip);
    void session_active(bool active);
    void restore(const std::vector<Connector*>& conns);

    Session& session_;
    Session::Device* device_ = nullptr;
    int fd_ = -1;
    std::string name_;
    wl_event_source* event_source_ = nullptr;
    std::vector<wl::Connection> connections_;

    uint64_t cursor_width_ = 64, cursor_height_ = 64;
    bool addfb2_modifiers_ = false;
    bool tearing_ = false;
    bool timeline_ = false;
    bool started_ = false;

    std::vector<std::unique_ptr<Crtc>> crtcs_;
    std::vector<std::unique_ptr<Plane>> planes_;
    std::vector<std::unique_ptr<Connector>> connectors_;
    std::vector<PageFlip*> page_flips_;

    struct Fb;
    std::unordered_map<wlr_buffer*, std::unique_ptr<Fb>> fbs_;
};

} // namespace atrium::backend::drm
