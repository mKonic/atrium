#pragma once
// Real screens: a GPU's KMS device, driven with atomic commits. Each
// connected connector is an Output; a CRTC is taken for it while it is on,
// its frames go to the CRTC's primary plane and the pointer to its cursor
// plane. After wlroots' backend/drm (MIT), with aquamarine's and KWin's
// as further reference.
#include "backend/allocator.hpp"
#include "backend/backend.hpp"
#include "backend/drm/props.hpp"
#include "backend/session.hpp"
#include "util/format_set.hpp"
#include "util/timeline.hpp"

#include <xf86drmMode.h>

#include <memory>
#include <unordered_map>
#include <vector>


namespace atrium::render {
class Renderer;
}

namespace atrium::backend::drm {

class Drm final : public Backend {
public:
    // The KMS device at `path`, opened through the session; null if it has
    // no display outputs or can't be driven. With a `parent` (the GPU that
    // renders), frames arrive from it and are copied over before scan-out.
    static std::unique_ptr<Drm> create(wl_event_loop* loop, Session& session, const std::string& path,
                                       Drm* parent = nullptr);
    ~Drm() override;

    bool start() override;
    int drm_fd() const override { return fd_; }
    uint32_t buffer_caps() const override;
    bool commit(const std::vector<std::pair<Output*, OutputState>>& states, bool test_only) override;
    bool is_drm() const override { return true; }
    bool supports_timelines() const override;

    const std::string& name() const { return name_; }
    Drm* parent() const { return parent_; }

    // Its device was unplugged.
    wl::Signal<> removed;

    // Leasing screens to clients (a VR runtime): the connector behind an
    // output, a fd without master to show clients, a lease's fd (-1 if
    // refused), and its end.
    uint32_t connector_id(const Output* output) const;
    int non_master_fd() const;
    int create_lease(const std::vector<uint32_t>& connectors, uint32_t* lessee);
    void revoke_lease(uint32_t lessee);
    // The kernel ended one (the lessee closed it).
    wl::Signal<uint32_t> lease_ended;

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
    uint32_t fb_for(Buffer* buffer, const FormatSet* formats);

    bool commit_connector(Connector& c, const OutputState& state, bool test_only);
    bool commit_states(std::vector<ConnState>& states, bool modeset, bool nonblock, bool test_only, bool async);
    bool prepare(ConnState& st, bool modeset, bool test_only);
    bool legacy_commit(std::vector<ConnState>& states, bool modeset, bool test_only, bool async, PageFlip* flip);
    uint32_t current_crtc(uint32_t connector, const drmModeConnector* info) const;
    bool init_mgpu();
    // `src` (the parent GPU's) drawn into a buffer of ours from `sc`, locked;
    // `fence` gets a sync_file for the copy's end when timelines work.
    // `from` is the renderer that drew it, for copies through the CPU.
    Buffer* copy_in(Buffer* src, std::unique_ptr<Swapchain>& sc, const FormatSet* formats,
                        render::Renderer* from, Timeline* wait, uint64_t wait_point, int* fence);
    bool cpu_copy(Buffer* src, Buffer* dst, render::Renderer* from, Timeline* wait,
                  uint64_t wait_point);
    void handle_page_flip(unsigned seq, unsigned sec, unsigned usec, unsigned crtc_id, PageFlip* flip);
    void session_active(bool active);
    void check_leases();
    void end_lease(uint32_t lessee, bool revoke);
    void restore(const std::vector<Connector*>& conns);

    Session& session_;
    Drm* parent_ = nullptr;
    // A secondary GPU's own renderer for the copies, what it can read of
    // the parent's buffers (explicit modifiers), and its copies' timeline.
    render::Renderer* mgpu_renderer_ = nullptr;
    std::unique_ptr<Allocator> mgpu_allocator_;
    std::unique_ptr<Allocator> mgpu_dumb_;  // copies through the CPU
    FormatSet mgpu_formats_{};
    Timeline* mgpu_timeline_ = nullptr;
    uint64_t mgpu_point_ = 0;
    bool mgpu_cpu_ = false;  // it can't read the parent's buffers
    Session::Device* device_ = nullptr;
    int fd_ = -1;
    std::string name_;
    wl_event_source* event_source_ = nullptr;
    std::vector<wl::Connection> connections_;

    uint64_t cursor_width_ = 64, cursor_height_ = 64;
    bool addfb2_modifiers_ = false;
    bool atomic_ = true;
    bool tearing_ = false;
    bool timeline_ = false;
    bool started_ = false;

    std::vector<std::unique_ptr<Crtc>> crtcs_;
    std::vector<std::unique_ptr<Plane>> planes_;
    std::vector<std::unique_ptr<Connector>> connectors_;
    std::vector<PageFlip*> page_flips_;

    struct Fb;
    std::unordered_map<Buffer*, std::unique_ptr<Fb>> fbs_;
};

} // namespace atrium::backend::drm
