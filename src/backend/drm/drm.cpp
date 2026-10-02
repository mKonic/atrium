#include "backend/drm/drm.hpp"

#include "backend/drm/match.hpp"
#include "listener.hpp"
#include "wlr.hpp"

extern "C" {
#include <libdisplay-info/cvt.h>
#include <libdisplay-info/info.h>
#include <wlr/render/dmabuf.h>
#include <wlr/util/transform.h>
}

#include <drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace atrium::backend::drm {

namespace {

// wp_presentation_feedback.kind
constexpr uint32_t kPresentVsync = 0x1, kPresentHwClock = 0x2, kPresentHwCompletion = 0x4;

int32_t refresh_of(const drmModeModeInfo& m) {
    int32_t r = int32_t((m.clock * 1000000LL / m.htotal + m.vtotal / 2) / m.vtotal);
    if (m.flags & DRM_MODE_FLAG_INTERLACE)
        r *= 2;
    if (m.flags & DRM_MODE_FLAG_DBLSCAN)
        r /= 2;
    if (m.vscan > 1)
        r /= m.vscan;
    return r;
}

// A mode for any size (a custom one): VESA CVT timings.
drmModeModeInfo cvt_mode(int w, int h, float refresh_hz) {
    di_cvt_options o{};
    o.red_blank_ver = DI_CVT_REDUCED_BLANKING_NONE;
    o.h_pixels = w;
    o.v_lines = h;
    o.ip_freq_rqd = refresh_hz > 0 ? refresh_hz : 60;
    di_cvt_timing t{};
    di_cvt_compute(&t, &o);
    const auto hs = uint16_t(w + t.h_front_porch);
    const auto vs = uint16_t(t.v_lines_rnd + t.v_front_porch);
    const auto he = uint16_t(hs + t.h_sync);
    const auto ve = uint16_t(vs + t.v_sync);
    drmModeModeInfo m{};
    m.clock = uint32_t(std::lround(t.act_pixel_freq * 1000));
    m.hdisplay = uint16_t(w);
    m.vdisplay = uint16_t(t.v_lines_rnd);
    m.hsync_start = hs;
    m.vsync_start = vs;
    m.hsync_end = he;
    m.vsync_end = ve;
    m.htotal = uint16_t(he + t.h_back_porch);
    m.vtotal = uint16_t(ve + t.v_back_porch);
    m.vrefresh = uint32_t(std::lround(t.act_frame_rate));
    m.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
    m.type = DRM_MODE_TYPE_USERDEF;
    std::snprintf(m.name, sizeof(m.name), "%dx%d", w, h);
    return m;
}

void destroy_blob(int fd, uint32_t id) {
    if (id)
        drmModeDestroyPropertyBlob(fd, id);
}

uint16_t cta_coord(double v) {
    return uint16_t(std::lround(std::clamp(v, 0.0, 1.0) * 50000));
}

uint64_t max_bpc_for(uint32_t format) {
    switch (format) {
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010:
        return 10;
    case DRM_FORMAT_XBGR16161616F:
    case DRM_FORMAT_ABGR16161616F:
    case DRM_FORMAT_XBGR16161616:
    case DRM_FORMAT_ABGR16161616:
        return 16;
    default:
        return 8;
    }
}

wlr_fbox src_box_of(const OutputState& s) {
    wlr_fbox b = s.buffer_src_box;
    if (b.width == 0 && b.height == 0)
        b = {0, 0, double(s.buffer->width), double(s.buffer->height)};
    return b;
}

wlr_box dst_box_of(const OutputState& s, int w, int h) {
    wlr_box b = s.buffer_dst_box;
    if (b.width == 0 && b.height == 0)
        b = {b.x, b.y, w, h};
    return b;
}

} // namespace

// ---- the KMS objects -------------------------------------------------------------------

struct Drm::Fb {
    uint32_t id = 0;
    bool poisoned = false;  // KMS refused it: not tried again
    Listener<> destroy;
};

struct Drm::Plane {
    uint32_t id = 0;
    uint64_t type = 0;
    PlaneProps props;
    wlr_drm_format_set formats{};
    std::vector<std::pair<int, int>> cursor_sizes;
    // Locked while KMS may show them: the one on screen, and the next.
    wlr_buffer* current = nullptr;
    wlr_buffer* queued = nullptr;
    wlr_fbox src{};
    wlr_box dst{};
    // Signalled once the buffer stops being shown (explicit sync).
    wlr_drm_syncobj_timeline* current_release = nullptr;
    uint64_t current_point = 0;
    wlr_drm_syncobj_timeline* queued_release = nullptr;
    uint64_t queued_point = 0;

    void clear() {
        for (wlr_buffer** b : {&current, &queued})
            if (*b) {
                wlr_buffer_unlock(*b);
                *b = nullptr;
            }
        for (auto* t : {&current_release, &queued_release})
            if (*t) {
                wlr_drm_syncobj_timeline_unref(*t);
                *t = nullptr;
            }
    }
    ~Plane() {
        clear();
        wlr_drm_format_set_finish(&formats);
    }
};

struct Drm::Crtc {
    uint32_t id = 0;
    size_t index = 0;         // in crtcs_
    size_t kernel_index = 0;  // in the kernel's list (possible_crtcs bits)
    CrtcProps props;
    Plane* primary = nullptr;
    Plane* cursor = nullptr;
    uint32_t mode_id = 0;
    bool own_mode_id = false;  // ours to destroy (not the previous master's)
    uint32_t gamma_lut = 0;
    int legacy_gamma_size = 0;
};

struct Drm::Connector {
    uint32_t id = 0;
    std::string name;
    ConnectorProps props;
    uint32_t possible_crtcs = 0;
    Crtc* crtc = nullptr;
    drmModeConnection status = DRM_MODE_DISCONNECTED;
    ConnOutput* output = nullptr;
    std::vector<drmModeModeInfo> modes;  // alongside output->modes
    int32_t refresh = 0;
    uint64_t max_bpc_min = 0, max_bpc_max = 0;
    uint64_t colorspace = 0;
    uint32_t hdr_metadata = 0;
    PageFlip* pending_flip = nullptr;

    // The cursor plane: what to show next, where (crtc pixels, hotspot taken off).
    bool cursor_enabled = false;
    wlr_buffer* cursor_pending = nullptr;
    int cursor_x = 0, cursor_y = 0, hotspot_x = 0, hotspot_y = 0, cursor_w = 0, cursor_h = 0;

    ~Connector() {
        if (cursor_pending)
            wlr_buffer_unlock(cursor_pending);
    }
};

struct Drm::PageFlip {
    Drm* drm = nullptr;
    std::vector<std::pair<Connector*, uint32_t>> conns;  // and their CRTCs
    bool async = false;
};

// One connector's part of a commit.
struct Drm::ConnState {
    Connector* conn = nullptr;
    const OutputState* base = nullptr;
    bool active = false;
    drmModeModeInfo mode{};
    wlr_buffer* primary = nullptr;  // locked
    uint32_t primary_fb = 0;
    wlr_fbox src{};
    wlr_box dst{};
    wlr_buffer* cursor = nullptr;  // locked
    uint32_t cursor_fb = 0;
    wlr_drm_syncobj_timeline* wait = nullptr;
    uint64_t wait_point = 0;
    uint32_t mode_id = 0, gamma_lut = 0, damage_clips = 0, hdr_metadata = 0;
    int in_fence = -1;
    bool vrr = false;
    uint64_t colorspace = 0;

    void finish() {
        for (wlr_buffer** b : {&primary, &cursor})
            if (*b) {
                wlr_buffer_unlock(*b);
                *b = nullptr;
            }
        if (wait) {
            wlr_drm_syncobj_timeline_unref(wait);
            wait = nullptr;
        }
    }
};

// ---- a connected screen ------------------------------------------------------------

class Drm::ConnOutput final : public Output {
public:
    ConnOutput(Drm& d, Connector& c) : Output(d), drm(d), conn(c) {}

    size_t gamma_size() const override {
        if (!conn.crtc)
            return 0;
        uint64_t size = 0;
        if (conn.crtc->props.gamma_lut_size && get_prop(drm.fd_, conn.crtc->id, conn.crtc->props.gamma_lut_size, &size))
            return size_t(size);
        return size_t(conn.crtc->legacy_gamma_size);
    }
    const wlr_drm_format_set* primary_formats(uint32_t) const override {
        return drm.alloc_crtc(conn) ? &conn.crtc->primary->formats : nullptr;
    }
    bool direct_scanout_allowed() const override { return true; }

    bool has_cursor_plane() const override { return drm.alloc_crtc(conn) && conn.crtc->cursor; }
    std::vector<std::pair<int, int>> cursor_sizes() const override {
        return has_cursor_plane() ? conn.crtc->cursor->cursor_sizes : std::vector<std::pair<int, int>>{};
    }
    const wlr_drm_format_set* cursor_formats(uint32_t) const override {
        return has_cursor_plane() ? &conn.crtc->cursor->formats : nullptr;
    }

    bool set_cursor(wlr_buffer* buffer, int hx, int hy) override {
        if (!conn.crtc || !conn.crtc->cursor)
            return false;
        Plane* plane = conn.crtc->cursor;
        if (conn.hotspot_x != hx || conn.hotspot_y != hy) {
            conn.cursor_x -= hx - conn.hotspot_x;
            conn.cursor_y -= hy - conn.hotspot_y;
            conn.hotspot_x = hx;
            conn.hotspot_y = hy;
        }
        conn.cursor_enabled = false;
        if (conn.cursor_pending) {
            wlr_buffer_unlock(conn.cursor_pending);
            conn.cursor_pending = nullptr;
        }
        if (buffer) {
            const bool fits = std::ranges::any_of(plane->cursor_sizes, [&](auto s) {
                return s.first == buffer->width && s.second == buffer->height;
            });
            if (!fits || !drm.fb_for(buffer, &plane->formats))
                return false;
            conn.cursor_pending = wlr_buffer_lock(buffer);
            conn.cursor_enabled = true;
            conn.cursor_w = buffer->width;
            conn.cursor_h = buffer->height;
        }
        return true;
    }

    bool move_cursor(int x, int y) override {
        if (!conn.crtc || !conn.crtc->cursor)
            return false;
        int w, h;
        transformed_resolution(&w, &h);
        wlr_box b{x, y, 0, 0};
        wlr_box_transform(&b, &b, wlr_output_transform_invert(transform), w, h);
        conn.cursor_x = b.x - conn.hotspot_x;
        conn.cursor_y = b.y - conn.hotspot_y;
        return true;
    }

    bool cursor_visible() const {
        return conn.cursor_enabled && conn.cursor_x < width && conn.cursor_y < height &&
               conn.cursor_x + conn.cursor_w >= 0 && conn.cursor_y + conn.cursor_h >= 0;
    }

    Drm& drm;
    Connector& conn;

protected:
    bool test(const OutputState& s) override { return drm.commit_connector(conn, s, true); }
    bool commit(const OutputState& s) override { return drm.commit_connector(conn, s, false); }
};

// ---- setup -----------------------------------------------------------------------------

Drm::Drm(wl_event_loop* loop, Session& session) : Backend(loop), session_(session) {}

std::unique_ptr<Drm> Drm::create(wl_event_loop* loop, Session& session, const std::string& path) {
    std::unique_ptr<Drm> d(new Drm(loop, session));
    d->device_ = session.open(path);
    if (!d->device_)
        return nullptr;
    d->fd_ = d->device_->fd;
    if (!drmIsKMS(d->fd_)) {
        wlr_log(WLR_INFO, "drm: %s has no display outputs", path.c_str());
        return nullptr;
    }
    drmVersion* v = drmGetVersion(d->fd_);
    d->name_ = path + (v ? std::string(" (") + v->name + ")" : "");
    if (v)
        drmFreeVersion(v);
    if (!d->check_features() || !d->init_resources())
        return nullptr;

    d->event_source_ = wl_event_loop_add_fd(
        loop, d->fd_, WL_EVENT_READABLE,
        [](int fd, uint32_t, void* data) {
            drmEventContext ev{};
            ev.version = 3;
            ev.page_flip_handler2 = [](int, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void* user) {
                auto* flip = static_cast<PageFlip*>(user);
                flip->drm->handle_page_flip(seq, sec, usec, crtc, flip);
            };
            if (drmHandleEvent(fd, &ev) != 0)
                wlr_log(WLR_ERROR, "drm: drmHandleEvent failed");
            (void)data;
            return 1;
        },
        d.get());

    d->connections_.push_back(session.events.active.connect([raw = d.get()](bool on) { raw->session_active(on); }));
    d->connections_.push_back(d->device_->events.change.connect([raw = d.get()](const Session::Device::Change& c) {
        if (!raw->session_.active())
            return;
        if (c.type == Session::Device::Change::Type::Hotplug)
            raw->scan_connectors(c.connector);
    }));
    wlr_log(WLR_INFO, "drm: driving %s", d->name_.c_str());
    return d;
}

Drm::~Drm() {
    connections_.clear();
    for (auto& c : connectors_)
        if (c->output)
            disconnect(*c);
    connectors_.clear();
    for (PageFlip* f : page_flips_)
        delete f;
    page_flips_.clear();
    for (auto& c : crtcs_) {
        if (c->own_mode_id)
            destroy_blob(fd_, c->mode_id);
        destroy_blob(fd_, c->gamma_lut);
    }
    planes_.clear();
    crtcs_.clear();
    for (auto& [buf, fb] : fbs_) {
        fb->destroy.disconnect();
        if (fb->id && drmModeCloseFB(fd_, fb->id) != 0)
            drmModeRmFB(fd_, fb->id);
    }
    fbs_.clear();
    if (event_source_)
        wl_event_source_remove(event_source_);
    if (device_)
        session_.close(device_);
    events.destroy.emit();
}

bool Drm::check_features() {
    if (drmGetCap(fd_, DRM_CAP_CURSOR_WIDTH, &cursor_width_))
        cursor_width_ = 64;
    if (drmGetCap(fd_, DRM_CAP_CURSOR_HEIGHT, &cursor_height_))
        cursor_height_ = 64;
    uint64_t cap = 0;
    if (drmGetCap(fd_, DRM_CAP_PRIME, &cap) || !(cap & DRM_PRIME_CAP_IMPORT)) {
        wlr_log(WLR_ERROR, "drm: %s can't import buffers (PRIME)", name_.c_str());
        return false;
    }
    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        wlr_log(WLR_ERROR, "drm: %s lacks universal planes", name_.c_str());
        return false;
    }
    if (drmGetCap(fd_, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) || !cap) {
        wlr_log(WLR_ERROR, "drm: %s lacks monotonic timestamps", name_.c_str());
        return false;
    }
    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1)) {
        wlr_log(WLR_ERROR, "drm: %s has no atomic modesetting (ATRIUM_WLR_DRM=1 drives it through wlroots)",
                name_.c_str());
        return false;
    }
    // Virtual GPUs place the pointer by its hotspot.
    drmSetClientCap(fd_, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1);
    tearing_ = drmGetCap(fd_, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
    timeline_ = drmGetCap(fd_, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1;
    addfb2_modifiers_ = drmGetCap(fd_, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap == 1;
    return true;
}

bool Drm::init_resources() {
    drmModeRes* res = drmModeGetResources(fd_);
    if (!res)
        return false;
    for (int i = 0; i < res->count_crtcs; ++i) {
        auto c = std::make_unique<Crtc>();
        c->id = res->crtcs[i];
        c->index = c->kernel_index = size_t(i);
        if (drmModeCrtc* info = drmModeGetCrtc(fd_, c->id)) {
            c->legacy_gamma_size = info->gamma_size;
            drmModeFreeCrtc(info);
        }
        get_props(fd_, c->id, &c->props);
        crtcs_.push_back(std::move(c));
    }
    drmModeFreeResources(res);

    drmModePlaneRes* pres = drmModeGetPlaneResources(fd_);
    if (!pres)
        return false;
    for (uint32_t i = 0; i < pres->count_planes; ++i) {
        drmModePlane* info = drmModeGetPlane(fd_, pres->planes[i]);
        if (!info)
            continue;
        auto p = std::make_unique<Plane>();
        p->id = info->plane_id;
        get_props(fd_, p->id, &p->props);
        get_prop(fd_, p->id, p->props.type, &p->type);
        for (uint32_t f = 0; f < info->count_formats; ++f) {
            // Without modifiers the cursor takes linear buffers only.
            wlr_drm_format_set_add(&p->formats, info->formats[f], DRM_FORMAT_MOD_LINEAR);
            if (p->type != DRM_PLANE_TYPE_CURSOR)
                wlr_drm_format_set_add(&p->formats, info->formats[f], DRM_FORMAT_MOD_INVALID);
        }
        uint64_t blob_id = 0;
        if (p->props.in_formats && addfb2_modifiers_ && get_prop(fd_, p->id, p->props.in_formats, &blob_id) && blob_id) {
            if (drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd_, uint32_t(blob_id))) {
                drmModeFormatModifierIterator it{};
                while (drmModeFormatModifierBlobIterNext(blob, &it))
                    wlr_drm_format_set_add(&p->formats, it.fmt, it.mod);
                drmModeFreePropertyBlob(blob);
            }
        }
        uint64_t hints_id = 0;
        if (p->props.size_hints && get_prop(fd_, p->id, p->props.size_hints, &hints_id) && hints_id) {
            if (drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd_, uint32_t(hints_id))) {
                const auto* h = static_cast<const drm_plane_size_hint*>(blob->data);
                for (size_t k = 0; k < blob->length / sizeof(*h); ++k)
                    p->cursor_sizes.emplace_back(h[k].width, h[k].height);
                drmModeFreePropertyBlob(blob);
            }
        }
        if (p->cursor_sizes.empty())
            p->cursor_sizes.emplace_back(int(cursor_width_), int(cursor_height_));
        for (auto& c : crtcs_) {
            if (!(info->possible_crtcs & (1u << c->kernel_index)))
                continue;
            if (p->type == DRM_PLANE_TYPE_PRIMARY && !c->primary) {
                c->primary = p.get();
                break;
            }
            if (p->type == DRM_PLANE_TYPE_CURSOR && !c->cursor) {
                c->cursor = p.get();
                break;
            }
        }
        drmModeFreePlane(info);
        planes_.push_back(std::move(p));
    }
    drmModeFreePlaneResources(pres);
    // A CRTC without a primary plane can't show anything.
    std::erase_if(crtcs_, [](const auto& c) { return !c->primary; });
    for (size_t i = 0; i < crtcs_.size(); ++i)
        crtcs_[i]->index = i;
    wlr_log(WLR_INFO, "drm: %zu CRTCs, %zu planes", crtcs_.size(), planes_.size());
    return !crtcs_.empty();
}

uint32_t Drm::buffer_caps() const {
    return WLR_BUFFER_CAP_DMABUF;
}

bool Drm::start() {
    started_ = true;
    scan_connectors();
    return true;
}

// ---- connectors ------------------------------------------------------------------------

void Drm::scan_connectors(uint32_t only) {
    drmModeRes* res = drmModeGetResources(fd_);
    if (!res)
        return;
    std::vector<Connector*> seen, fresh;
    for (int i = 0; i < res->count_connectors; ++i) {
        const uint32_t cid = res->connectors[i];
        auto it = std::ranges::find_if(connectors_, [cid](const auto& c) { return c->id == cid; });
        Connector* c = it == connectors_.end() ? nullptr : it->get();
        if (only && cid != only) {
            if (c)
                seen.push_back(c);
            continue;
        }
        drmModeConnector* info = drmModeGetConnector(fd_, cid);
        if (!info)
            continue;
        if (!c) {
            auto made = std::make_unique<Connector>();
            made->id = cid;
            get_props(fd_, cid, &made->props);
            const char* type = drmModeGetConnectorTypeName(info->connector_type);
            made->name = std::string(type ? type : "Unknown") + "-" + std::to_string(info->connector_type_id);
            made->possible_crtcs = drmModeConnectorGetPossibleCrtcs(fd_, info);
            // Which CRTCs a CRTC's index stands for, ours being a subset.
            uint64_t current = 0;
            if (get_prop(fd_, cid, made->props.crtc_id, &current) && current)
                for (auto& cr : crtcs_)
                    if (cr->id == current)
                        made->crtc = cr.get();
            c = made.get();
            connectors_.push_back(std::move(made));
            wlr_log(WLR_INFO, "drm: found connector %s", c->name.c_str());
        }
        seen.push_back(c);
        // A link gone bad (a DP cable wiggled): its modes are read again and
        // it is set up anew.
        uint64_t link = 0;
        if (c->props.link_status && get_prop(fd_, cid, c->props.link_status, &link) &&
            link == DRM_MODE_LINK_STATUS_BAD && c->output) {
            wlr_log(WLR_INFO, "drm: %s: bad link", c->name.c_str());
            disconnect(*c);
        }
        if (!c->output && info->connection == DRM_MODE_CONNECTED) {
            wlr_log(WLR_INFO, "drm: %s connected", c->name.c_str());
            if (connect(*c, *info))
                fresh.push_back(c);
        } else if (c->output && info->connection != DRM_MODE_CONNECTED) {
            wlr_log(WLR_INFO, "drm: %s disconnected", c->name.c_str());
            disconnect(*c);
        }
        drmModeFreeConnector(info);
    }
    drmModeFreeResources(res);
    // Connectors that went away (an MST hub unplugged).
    std::erase_if(connectors_, [&](const std::unique_ptr<Connector>& c) {
        if (std::ranges::find(seen, c.get()) != seen.end())
            return false;
        if (c->output)
            disconnect(*c);
        return true;
    });
    if (started_)
        for (Connector* c : fresh)
            events.new_output.emit(c->output);
}

bool Drm::connect(Connector& c, const drmModeConnector& info) {
    auto* o = new ConnOutput(*this, c);
    c.output = o;
    o->name = c.name;
    o->phys_width = int(info.mmWidth);
    o->phys_height = int(info.mmHeight);
    static constexpr wl_output_subpixel kSubpixel[] = {
        WL_OUTPUT_SUBPIXEL_UNKNOWN,        WL_OUTPUT_SUBPIXEL_UNKNOWN,        WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB,
        WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR, WL_OUTPUT_SUBPIXEL_VERTICAL_RGB,   WL_OUTPUT_SUBPIXEL_VERTICAL_BGR,
        WL_OUTPUT_SUBPIXEL_NONE,
    };
    if (size_t(info.subpixel) < std::size(kSubpixel))
        o->subpixel = kSubpixel[info.subpixel];

    // What the CRTC shows now (the boot splash's mode): kept, no modeset.
    drmModeModeInfo current{};
    bool has_current = false;
    if (c.crtc) {
        const std::vector<uint8_t> blob = get_prop_blob(fd_, c.crtc->id, c.crtc->props.mode_id);
        if (blob.size() == sizeof(drmModeModeInfo)) {
            std::memcpy(&current, blob.data(), sizeof(current));
            has_current = true;
            uint64_t id = 0;
            get_prop(fd_, c.crtc->id, c.crtc->props.mode_id, &id);
            c.crtc->mode_id = uint32_t(id);
            c.crtc->own_mode_id = false;
        }
    }
    c.modes.clear();
    o->modes.clear();
    for (int i = 0; i < info.count_modes; ++i) {
        const drmModeModeInfo& m = info.modes[i];
        if (m.flags & DRM_MODE_FLAG_INTERLACE)
            continue;
        c.modes.push_back(m);
        o->modes.push_back({m.hdisplay, m.vdisplay, refresh_of(m), bool(m.type & DRM_MODE_TYPE_PREFERRED),
                            uint64_t(c.modes.size() - 1)});
    }
    o->enabled = c.crtc != nullptr && has_current;
    if (has_current) {
        for (size_t i = 0; i < c.modes.size(); ++i)
            if (std::memcmp(&c.modes[i], &current, sizeof(current)) == 0)
                o->current_mode = &o->modes[i];
        o->width = current.hdisplay;
        o->height = current.vdisplay;
        o->refresh = refresh_of(current);
        c.refresh = o->refresh;
    }

    uint64_t v = 0;
    if (get_prop(fd_, c.id, c.props.non_desktop, &v))
        o->non_desktop = v == 1;
    c.max_bpc_min = c.max_bpc_max = 0;
    if (c.props.max_bpc)
        prop_range(fd_, c.props.max_bpc, &c.max_bpc_min, &c.max_bpc_max);
    v = 0;
    if (c.props.vrr_capable)
        get_prop(fd_, c.id, c.props.vrr_capable, &v);
    o->adaptive_sync_supported = v == 1;

    const std::vector<uint8_t> edid = get_prop_blob(fd_, c.id, c.props.edid);
    if (!edid.empty())
        if (di_info* di = di_info_parse_edid(edid.data(), edid.size())) {
            auto take = [](char* s) {
                std::string out = s ? s : "";
                std::free(s);
                return out;
            };
            o->make = take(di_info_get_make(di));
            o->model = take(di_info_get_model(di));
            o->serial = take(di_info_get_serial(di));
            const di_color_primaries* p = di_info_get_default_color_primaries(di);
            if (p->has_primaries)
                o->default_primaries = wlr_color_primaries{{p->primary[0].x, p->primary[0].y},
                                                           {p->primary[1].x, p->primary[1].y},
                                                           {p->primary[2].x, p->primary[2].y},
                                                           {p->default_white.x, p->default_white.y}};
            const di_supported_signal_colorimetry* col = di_info_get_supported_signal_colorimetry(di);
            if (c.props.colorspace && (col->bt2020_cycc || col->bt2020_ycc || col->bt2020_rgb))
                o->supported_primaries |= WLR_COLOR_NAMED_PRIMARIES_BT2020;
            const di_hdr_static_metadata* hdr = di_info_get_hdr_static_metadata(di);
            if (c.props.hdr_output_metadata && hdr->type1 && hdr->pq)
                o->supported_transfer_functions |= WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
            di_info_destroy(di);
        }
    std::string sub;
    if (c.props.subconnector) {
        sub = get_prop_enum(fd_, c.id, c.props.subconnector);
        if (sub == "Native")
            sub.clear();
    }
    o->description = o->make + (o->model.empty() ? "" : " " + o->model) +
                     (o->serial.empty() ? "" : " " + o->serial) + " (" + c.name + (sub.empty() ? "" : " via " + sub) +
                     ")";
    c.status = DRM_MODE_CONNECTED;
    return true;
}

void Drm::disconnect(Connector& c) {
    if (!c.output)
        return;
    // Off first (it may fail: the GPU unplugged, or another VT has it).
    if (c.crtc) {
        OutputState off;
        off.set_enabled(false);
        commit_connector(c, off, false);
        if (c.crtc) {
            c.crtc->primary->clear();
            if (c.crtc->cursor)
                c.crtc->cursor->clear();
        }
        c.crtc = nullptr;
    }
    for (PageFlip* f : page_flips_)
        for (auto& [conn, crtc] : f->conns)
            if (conn == &c)
                conn = nullptr;
    c.pending_flip = nullptr;
    c.cursor_enabled = false;
    if (c.cursor_pending) {
        wlr_buffer_unlock(c.cursor_pending);
        c.cursor_pending = nullptr;
    }
    destroy_blob(fd_, c.hdr_metadata);
    c.hdr_metadata = 0;
    ConnOutput* o = c.output;
    c.output = nullptr;
    c.status = DRM_MODE_DISCONNECTED;
    o->events.destroy.emit();
    delete o;
}

void Drm::realloc_crtcs(Connector* want) {
    if (connectors_.empty())
        return;
    std::vector<uint32_t> possible(connectors_.size(), 0);
    std::vector<uint32_t> previous(crtcs_.size(), kUnmatched);
    for (size_t i = 0; i < connectors_.size(); ++i) {
        Connector& c = *connectors_[i];
        if (c.crtc)
            previous[c.crtc->index] = uint32_t(i);
        // Only for those on, or the one about to be.
        const bool wants = &c == want || (c.output && c.output->enabled);
        if (c.output && wants) {
            // possible_crtcs counts the kernel's CRTCs; ours may be fewer.
            for (auto& cr : crtcs_)
                if (c.possible_crtcs & (1u << cr->kernel_index))
                    possible[i] |= 1u << cr->index;
        }
    }
    const std::vector<uint32_t> got = match_crtcs(possible, previous);
    std::vector<Crtc*> match(connectors_.size(), nullptr);
    for (size_t k = 0; k < got.size(); ++k)
        if (got[k] != kUnmatched)
            match[got[k]] = crtcs_[k].get();
    // A screen that's on keeps its CRTC, or nothing changes.
    for (size_t i = 0; i < connectors_.size(); ++i) {
        Connector& c = *connectors_[i];
        if (!c.output || !c.output->enabled)
            continue;
        if (!match[i] || match[i] != c.crtc)
            return;
    }
    for (size_t i = 0; i < connectors_.size(); ++i) {
        Connector& c = *connectors_[i];
        if (c.crtc && match[i])
            continue;
        if (c.crtc && c.output) {
            OutputState off;
            off.set_enabled(false);
            commit_connector(c, off, false);
        }
        c.crtc = match[i];
    }
}

bool Drm::alloc_crtc(Connector& c) {
    if (!c.crtc)
        realloc_crtcs(&c);
    return c.crtc != nullptr;
}

// ---- framebuffers -------------------------------------------------------------------

uint32_t Drm::fb_for(wlr_buffer* buffer, const wlr_drm_format_set* formats) {
    if (auto it = fbs_.find(buffer); it != fbs_.end())
        return it->second->poisoned ? 0 : it->second->id;
    wlr_dmabuf_attributes a;
    if (!wlr_buffer_get_dmabuf(buffer, &a))
        return 0;
    if (formats && !wlr_drm_format_set_has(formats, a.format, a.modifier))
        return 0;  // not this plane's (another may take it)
    auto fb = std::make_unique<Fb>();
    uint32_t handles[4] = {};
    bool ok = true;
    for (int i = 0; i < a.n_planes; ++i)
        if (drmPrimeFDToHandle(fd_, a.fd[i], &handles[i]) != 0) {
            ok = false;
            break;
        }
    if (ok) {
        uint64_t mods[4] = {};
        for (int i = 0; i < a.n_planes; ++i)
            mods[i] = a.modifier;  // KMS wants one for all planes
        if (addfb2_modifiers_ && a.modifier != DRM_FORMAT_MOD_INVALID) {
            if (drmModeAddFB2WithModifiers(fd_, uint32_t(a.width), uint32_t(a.height), a.format, handles, a.stride,
                                           a.offset, mods, &fb->id, DRM_MODE_FB_MODIFIERS) != 0)
                fb->id = 0;
        } else if (a.modifier == DRM_FORMAT_MOD_INVALID || a.modifier == DRM_FORMAT_MOD_LINEAR) {
            if (drmModeAddFB2(fd_, uint32_t(a.width), uint32_t(a.height), a.format, handles, a.stride, a.offset,
                              &fb->id, 0) != 0)
                fb->id = 0;
        }
    }
    // The framebuffer holds the buffer object now.
    for (int i = 0; i < 4; ++i) {
        if (!handles[i])
            continue;
        if (std::find(handles, handles + i, handles[i]) == handles + i)
            drmCloseBufferHandle(fd_, handles[i]);
    }
    fb->poisoned = fb->id == 0;
    if (fb->poisoned)
        wlr_log(WLR_DEBUG, "drm: buffer 0x%x/0x%lx refused for scan-out", a.format, (unsigned long)a.modifier);
    const uint32_t id = fb->id;
    fb->destroy.connect(&buffer->events.destroy, [this, buffer](void*) {
        auto it = fbs_.find(buffer);
        if (it == fbs_.end())
            return;
        if (it->second->id && drmModeCloseFB(fd_, it->second->id) != 0)
            drmModeRmFB(fd_, it->second->id);
        fbs_.erase(it);
    });
    fbs_[buffer] = std::move(fb);
    return id;
}

// ---- commits ----------------------------------------------------------------------------

namespace {

constexpr uint32_t kKmsFields = OutputState::Buffer | OutputState::ModeField | OutputState::Enabled |
                                OutputState::AdaptiveSyncEnabled | OutputState::WaitTimeline |
                                OutputState::SignalTimeline | OutputState::ColorTransform |
                                OutputState::ImageDescriptionField;

bool pending_enabled(const Output& o, const OutputState& s) {
    return (s.committed & OutputState::Enabled) ? s.enabled : o.enabled;
}

} // namespace

bool Drm::commit_connector(Connector& c, const OutputState& state, bool test_only) {
    if (!session_.active() || !c.output)
        return false;
    if (test_only && !(state.committed & kKmsFields))
        return true;  // nothing KMS sees changes
    const bool on = pending_enabled(*c.output, state);
    if (on && !alloc_crtc(c)) {
        wlr_log(WLR_DEBUG, "drm: %s: no CRTC free", c.name.c_str());
        return false;
    }
    if (!on && !c.crtc)
        return true;  // off already
    const bool modeset = state.allow_reconfiguration;
    // A flip without a modeset waits for the previous one.
    const bool nonblock = !modeset && (state.committed & OutputState::Buffer);
    if (!test_only && nonblock && c.pending_flip) {
        wlr_log(WLR_ERROR, "drm: %s: a page flip is still pending", c.name.c_str());
        return false;
    }
    std::vector<ConnState> states(1);
    states[0].conn = &c;
    states[0].base = &state;
    return commit_states(states, modeset, nonblock, test_only, state.tearing_page_flip);
}

bool Drm::commit(const std::vector<std::pair<Output*, OutputState>>& in, bool test_only) {
    if (!session_.active())
        return false;
    // Each output's own checks and blank modeset buffers first.
    std::vector<std::pair<ConnOutput*, OutputState>> ready;
    for (const auto& [o, st] : in) {
        auto* co = static_cast<ConnOutput*>(o);
        ready.emplace_back(co, st);
        if (!co->prepare_commit(ready.back().second))
            return false;
    }
    std::vector<ConnState> states;
    bool modeset = false;
    for (auto& [co, st] : ready) {
        if (!co->enabled && !pending_enabled(*co, st))
            continue;  // KMS refuses turning off what's off, with a flip event
        if (pending_enabled(*co, st) && !alloc_crtc(co->conn))
            return false;
        if (st.tearing_page_flip)
            return false;
        ConnState cs;
        cs.conn = &co->conn;
        cs.base = &st;
        states.push_back(cs);
        modeset |= st.allow_reconfiguration;
    }
    if (!commit_states(states, modeset, false, test_only, false))
        return false;
    if (!test_only)
        for (auto& [co, st] : ready)
            co->finish_commit(st);
    return true;
}

// The connector's part: what it will show, and the blobs and fences for it.
bool Drm::prepare(ConnState& st, bool modeset) {
    Connector& c = *st.conn;
    const OutputState& s = *st.base;
    ConnOutput& o = *c.output;
    st.active = pending_enabled(o, s);

    // The mode.
    if (s.committed & OutputState::ModeField) {
        if (s.mode_type == OutputState::ModeType::Fixed && s.mode)
            st.mode = c.modes[size_t(s.mode->native)];
        else
            st.mode = cvt_mode(s.custom_mode.width, s.custom_mode.height, float(s.custom_mode.refresh) / 1000);
    } else if (o.current_mode) {
        st.mode = c.modes[size_t(o.current_mode->native)];
    } else if (o.width > 0 && o.height > 0) {
        st.mode = cvt_mode(o.width, o.height, float(o.refresh) / 1000);
    }
    if ((s.committed & OutputState::Enabled) && s.enabled && o.width == 0 && o.height == 0 &&
        !(s.committed & OutputState::ModeField))
        return false;  // on, without a mode
    if ((s.committed & OutputState::AdaptiveSyncEnabled) && s.adaptive_sync_enabled && !o.adaptive_sync_supported)
        return false;
    if (s.tearing_page_flip && !tearing_)
        return false;

    if (st.active) {
        Crtc& crtc = *c.crtc;
        Plane& primary = *crtc.primary;
        if (s.committed & OutputState::Buffer) {
            st.primary_fb = fb_for(s.buffer, &primary.formats);
            if (!st.primary_fb)
                return false;
            st.primary = wlr_buffer_lock(s.buffer);
            st.src = src_box_of(s);
            st.dst = dst_box_of(s, s.buffer->width, s.buffer->height);
            if ((s.committed & OutputState::WaitTimeline) && s.wait_timeline) {
                st.wait = wlr_drm_syncobj_timeline_ref(s.wait_timeline);
                st.wait_point = s.wait_point;
            }
        } else if (wlr_buffer* b = primary.queued ? primary.queued : primary.current) {
            st.primary_fb = fb_for(b, &primary.formats);
            st.primary = wlr_buffer_lock(b);
            st.src = primary.src;
            st.dst = primary.dst;
        }
        if (!st.primary_fb)
            return false;  // nothing to show
        if (c.cursor_enabled && crtc.cursor) {
            wlr_buffer* cb = c.cursor_pending ? c.cursor_pending
                             : crtc.cursor->queued ? crtc.cursor->queued
                                                   : crtc.cursor->current;
            if (cb) {
                st.cursor_fb = fb_for(cb, &crtc.cursor->formats);
                if (st.cursor_fb)
                    st.cursor = wlr_buffer_lock(cb);
            }
        }
    }

    Crtc* crtc = c.crtc;
    st.mode_id = crtc->mode_id;
    if (modeset) {
        st.mode_id = 0;
        if (st.active && drmModeCreatePropertyBlob(fd_, &st.mode, sizeof(st.mode), &st.mode_id) != 0)
            return false;
    }
    st.gamma_lut = crtc->gamma_lut;
    if (s.committed & OutputState::ColorTransform) {
        st.gamma_lut = 0;
        size_t n = 0;
        uint64_t size = 0;
        if (crtc->props.gamma_lut_size && get_prop(fd_, crtc->id, crtc->props.gamma_lut_size, &size))
            n = size_t(size);
        if (s.color_transform && n > 1 && crtc->props.gamma_lut) {
            std::vector<drm_color_lut> lut(n);
            for (size_t i = 0; i < n; ++i) {
                const float x = float(i) / float(n - 1);
                const float in[3] = {x, x, x};
                float out[3];
                wlr_color_transform_eval(s.color_transform, out, in);
                lut[i].red = uint16_t(std::lround(std::clamp(out[0], 0.0f, 1.0f) * 65535));
                lut[i].green = uint16_t(std::lround(std::clamp(out[1], 0.0f, 1.0f) * 65535));
                lut[i].blue = uint16_t(std::lround(std::clamp(out[2], 0.0f, 1.0f) * 65535));
            }
            if (drmModeCreatePropertyBlob(fd_, lut.data(), lut.size() * sizeof(lut[0]), &st.gamma_lut) != 0)
                return false;
        } else if (s.color_transform && !crtc->props.gamma_lut) {
            return false;  // no gamma table on this CRTC
        }
    }
    if ((s.committed & OutputState::Damage) && crtc->primary->props.fb_damage_clips && st.primary) {
        pixman_region32_t clipped;
        pixman_region32_init(&clipped);
        pixman_region32_intersect_rect(&clipped, &s.damage, 0, 0, unsigned(st.primary->width),
                                       unsigned(st.primary->height));
        int n = 0;
        const pixman_box32_t* rects = pixman_region32_rectangles(&clipped, &n);
        if (n > 0)
            drmModeCreatePropertyBlob(fd_, rects, sizeof(*rects) * size_t(n), &st.damage_clips);
        pixman_region32_fini(&clipped);
    }
    if (st.wait) {
        st.in_fence = wlr_drm_syncobj_timeline_export_sync_file(st.wait, st.wait_point);
        if (st.in_fence < 0)
            return false;
    }
    st.vrr = o.adaptive_sync_status == AdaptiveSync::Enabled;
    if (s.committed & OutputState::AdaptiveSyncEnabled)
        st.vrr = s.adaptive_sync_enabled;
    st.colorspace = c.colorspace;
    st.hdr_metadata = c.hdr_metadata;
    if (s.committed & OutputState::ImageDescriptionField) {
        const auto& d = s.image_description;
        st.colorspace = d && d->primaries == WLR_COLOR_NAMED_PRIMARIES_BT2020 ? 9 : 0;  // BT2020_RGB
        st.hdr_metadata = 0;
        if (d) {
            if (d->transfer_function != WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ)
                return false;  // what KMS can be told: PQ
            hdr_output_metadata md{};
            md.metadata_type = 0;
            auto& t1 = md.hdmi_metadata_type1;
            t1.eotf = 2;  // SMPTE ST 2084
            t1.metadata_type = 0;
            const wlr_color_primaries& p = d->mastering_display_primaries;
            t1.display_primaries[0] = {cta_coord(p.red.x), cta_coord(p.red.y)};
            t1.display_primaries[1] = {cta_coord(p.green.x), cta_coord(p.green.y)};
            t1.display_primaries[2] = {cta_coord(p.blue.x), cta_coord(p.blue.y)};
            t1.white_point = {cta_coord(p.white.x), cta_coord(p.white.y)};
            t1.max_display_mastering_luminance = uint16_t(d->mastering_luminance.max);
            t1.min_display_mastering_luminance = uint16_t(d->mastering_luminance.min * 0.0001);
            t1.max_cll = uint16_t(d->max_cll);
            t1.max_fall = uint16_t(d->max_fall);
            if (drmModeCreatePropertyBlob(fd_, &md, sizeof(md), &st.hdr_metadata) != 0)
                return false;
        }
    }
    return true;
}

bool Drm::commit_states(std::vector<ConnState>& states, bool modeset, bool nonblock, bool test_only, bool async) {
    bool ok = true;
    size_t prepared = 0;
    for (ConnState& st : states) {
        ++prepared;
        if (!prepare(st, modeset)) {
            ok = false;
            break;
        }
    }

    drmModeAtomicReq* req = ok ? drmModeAtomicAlloc() : nullptr;
    auto add = [&](uint32_t obj, uint32_t prop, uint64_t value) {
        if (ok && prop && drmModeAtomicAddProperty(req, obj, prop, value) < 0)
            ok = false;
    };
    auto plane_off = [&](Plane& p) {
        add(p.id, p.props.fb_id, 0);
        add(p.id, p.props.crtc_id, 0);
    };
    auto plane_on = [&](Plane& p, uint32_t fb, uint32_t crtc, const wlr_box& dst, const wlr_fbox& src) {
        // src_* in 16.16 fixed point.
        add(p.id, p.props.src_x, uint64_t(src.x * 65536));
        add(p.id, p.props.src_y, uint64_t(src.y * 65536));
        add(p.id, p.props.src_w, uint64_t(src.width * 65536));
        add(p.id, p.props.src_h, uint64_t(src.height * 65536));
        add(p.id, p.props.fb_id, fb);
        add(p.id, p.props.crtc_id, crtc);
        add(p.id, p.props.crtc_x, uint64_t(int64_t(dst.x)));
        add(p.id, p.props.crtc_y, uint64_t(int64_t(dst.y)));
        add(p.id, p.props.crtc_w, uint64_t(dst.width));
        add(p.id, p.props.crtc_h, uint64_t(dst.height));
    };
    if (ok)
        for (ConnState& st : states) {
            Connector& c = *st.conn;
            Crtc& crtc = *c.crtc;
            add(c.id, c.props.crtc_id, st.active ? crtc.id : 0);
            if (modeset && st.active && c.props.link_status)
                add(c.id, c.props.link_status, DRM_MODE_LINK_STATUS_GOOD);
            if (st.active && c.props.content_type)
                add(c.id, c.props.content_type, DRM_MODE_CONTENT_TYPE_GRAPHICS);
            if (modeset && st.active && c.props.max_bpc && c.max_bpc_max && st.primary) {
                wlr_dmabuf_attributes a;
                uint64_t bpc = max_bpc_for(wlr_buffer_get_dmabuf(st.primary, &a) ? a.format : 0);
                add(c.id, c.props.max_bpc, std::clamp(bpc, c.max_bpc_min, c.max_bpc_max));
            }
            if (c.props.colorspace)
                add(c.id, c.props.colorspace, st.colorspace);
            if (c.props.hdr_output_metadata)
                add(c.id, c.props.hdr_output_metadata, st.hdr_metadata);
            add(crtc.id, crtc.props.mode_id, st.mode_id);
            add(crtc.id, crtc.props.active, st.active);
            if (st.active) {
                if (crtc.props.gamma_lut)
                    add(crtc.id, crtc.props.gamma_lut, st.gamma_lut);
                if (crtc.props.vrr_enabled)
                    add(crtc.id, crtc.props.vrr_enabled, st.vrr);
                plane_on(*crtc.primary, st.primary_fb, crtc.id, st.dst, st.src);
                if (crtc.primary->props.fb_damage_clips)
                    add(crtc.primary->id, crtc.primary->props.fb_damage_clips, st.damage_clips);
                if (st.in_fence >= 0) {
                    if (!crtc.primary->props.in_fence_fd)
                        ok = false;
                    add(crtc.primary->id, crtc.primary->props.in_fence_fd, uint64_t(st.in_fence));
                }
                if (crtc.cursor) {
                    if (st.cursor && c.output->cursor_visible()) {
                        const wlr_fbox src{0, 0, double(st.cursor->width), double(st.cursor->height)};
                        const wlr_box dst{c.cursor_x, c.cursor_y, st.cursor->width, st.cursor->height};
                        plane_on(*crtc.cursor, st.cursor_fb, crtc.id, dst, src);
                        if (crtc.cursor->props.hotspot_x && crtc.cursor->props.hotspot_y) {
                            add(crtc.cursor->id, crtc.cursor->props.hotspot_x, uint64_t(c.hotspot_x));
                            add(crtc.cursor->id, crtc.cursor->props.hotspot_y, uint64_t(c.hotspot_y));
                        }
                    } else {
                        plane_off(*crtc.cursor);
                    }
                }
            } else {
                plane_off(*crtc.primary);
                if (crtc.cursor)
                    plane_off(*crtc.cursor);
            }
        }

    PageFlip* flip = nullptr;
    uint32_t flags = 0;
    if (!test_only && std::ranges::any_of(states, [](const ConnState& s) { return s.active; })) {
        flip = new PageFlip();
        flip->drm = this;
        flip->async = async;
        for (ConnState& st : states)
            flip->conns.emplace_back(st.conn, st.conn->crtc->id);
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
    }
    if (async)
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    if (test_only)
        flags |= DRM_MODE_ATOMIC_TEST_ONLY;
    if (modeset)
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (nonblock)
        flags |= DRM_MODE_ATOMIC_NONBLOCK;
    if (ok && drmModeAtomicCommit(fd_, req, flags, flip) != 0) {
        wlr_log(test_only ? WLR_DEBUG : WLR_ERROR, "drm: atomic commit (%s%s) failed: %s",
                states.size() == 1 ? states[0].conn->name.c_str() : "several screens",
                modeset ? ", modeset" : "", std::strerror(errno));
        ok = false;
    }
    if (req)
        drmModeAtomicFree(req);

    for (size_t i = 0; i < states.size(); ++i) {
        ConnState& st = states[i];
        Connector& c = *st.conn;
        if (ok && !test_only && i < prepared) {
            Crtc& crtc = *c.crtc;
            // The blobs now in use; the ones they replace go.
            if (crtc.mode_id != st.mode_id) {
                if (crtc.own_mode_id)
                    destroy_blob(fd_, crtc.mode_id);
                crtc.mode_id = st.mode_id;
            }
            crtc.own_mode_id = true;
            if (crtc.gamma_lut != st.gamma_lut) {
                destroy_blob(fd_, crtc.gamma_lut);
                crtc.gamma_lut = st.gamma_lut;
            }
            if (c.hdr_metadata != st.hdr_metadata) {
                destroy_blob(fd_, c.hdr_metadata);
                c.hdr_metadata = st.hdr_metadata;
            }
            c.colorspace = st.colorspace;
            c.output->adaptive_sync_status = st.vrr ? AdaptiveSync::Enabled : AdaptiveSync::Disabled;
            // What the screen will show next.
            Plane& primary = *crtc.primary;
            if (st.primary) {
                if (primary.queued)
                    wlr_buffer_unlock(primary.queued);
                primary.queued = std::exchange(st.primary, nullptr);
                primary.src = st.src;
                primary.dst = st.dst;
            }
            if (primary.queued_release) {
                // Replaced before it was ever shown.
                wlr_drm_syncobj_timeline_signal(primary.queued_release, primary.queued_point);
                wlr_drm_syncobj_timeline_unref(primary.queued_release);
                primary.queued_release = nullptr;
            }
            if ((st.base->committed & OutputState::SignalTimeline) && st.base->signal_timeline) {
                primary.queued_release = wlr_drm_syncobj_timeline_ref(st.base->signal_timeline);
                primary.queued_point = st.base->signal_point;
            }
            if (crtc.cursor) {
                if (crtc.cursor->queued)
                    wlr_buffer_unlock(crtc.cursor->queued);
                crtc.cursor->queued = std::exchange(st.cursor, nullptr);
            }
            if (c.cursor_pending) {
                wlr_buffer_unlock(c.cursor_pending);
                c.cursor_pending = nullptr;
            }
            c.pending_flip = flip;
            if (st.base->committed & OutputState::ModeField)
                c.refresh = refresh_of(st.mode);
            if (!st.active) {
                primary.clear();
                if (crtc.cursor)
                    crtc.cursor->clear();
                c.cursor_enabled = false;
                c.crtc = nullptr;
            }
        } else if (i < prepared) {
            // Rolled back: blobs made for it go.
            if (st.mode_id != c.crtc->mode_id)
                destroy_blob(fd_, st.mode_id);
            if (st.gamma_lut != c.crtc->gamma_lut)
                destroy_blob(fd_, st.gamma_lut);
            if (st.hdr_metadata != c.hdr_metadata)
                destroy_blob(fd_, st.hdr_metadata);
        }
        destroy_blob(fd_, st.damage_clips);
        if (st.in_fence >= 0)
            close(st.in_fence);
        st.finish();
    }
    if (flip) {
        if (ok)
            page_flips_.push_back(flip);
        else
            delete flip;
    }
    return ok;
}

void Drm::handle_page_flip(unsigned seq, unsigned sec, unsigned usec, unsigned crtc_id, PageFlip* flip) {
    Connector* c = nullptr;
    for (auto it = flip->conns.begin(); it != flip->conns.end(); ++it)
        if (it->second == crtc_id) {
            c = it->first;
            flip->conns.erase(it);
            break;
        }
    const bool async = flip->async;
    if (c && c->pending_flip == flip)
        c->pending_flip = nullptr;
    if (flip->conns.empty()) {
        std::erase(page_flips_, flip);
        delete flip;
    }
    if (!c)
        return;
    if (!c->output || !c->crtc)
        return;
    Plane& primary = *c->crtc->primary;
    if (primary.queued) {
        if (primary.current)
            wlr_buffer_unlock(primary.current);
        primary.current = std::exchange(primary.queued, nullptr);
        if (primary.current_release) {
            // No longer on screen: the client may reuse it.
            wlr_drm_syncobj_timeline_signal(primary.current_release, primary.current_point);
            wlr_drm_syncobj_timeline_unref(primary.current_release);
        }
        primary.current_release = std::exchange(primary.queued_release, nullptr);
        primary.current_point = primary.queued_point;
    }
    if (Plane* cur = c->crtc->cursor; cur && cur->queued) {
        if (cur->current)
            wlr_buffer_unlock(cur->current);
        cur->current = std::exchange(cur->queued, nullptr);
    }
    Present p;
    p.commit_seq = c->output->commit_seq;  // the last one: KMS shows them in order
    p.presented = session_.active();
    p.when.tv_sec = time_t(sec);
    p.when.tv_nsec = long(usec) * 1000;
    p.seq = seq;
    p.refresh = c->refresh > 0 ? int(1000000000000LL / c->refresh) : 0;
    p.flags = kPresentHwClock | kPresentHwCompletion | (async ? 0 : kPresentVsync);
    ConnOutput* o = c->output;
    o->send_present(p);
    if (session_.active() && c->output == o)
        o->send_frame();
}

void Drm::session_active(bool active) {
    wlr_log(WLR_INFO, "drm: %s %s", name_.c_str(), active ? "resumed" : "paused");
    // Paused: the screens stay as they are (flips in flight still complete,
    // unpresented). Back: what changed while away is found, the rest is
    // shown again as it was.
    if (!active)
        return;
    std::vector<Connector*> before;
    for (auto& c : connectors_)
        if (c->output && c->output->enabled && c->crtc)
            before.push_back(c.get());
    scan_connectors();
    std::erase_if(before, [&](Connector* c) {
        return std::ranges::find_if(connectors_, [c](const auto& x) { return x.get() == c; }) == connectors_.end() ||
               !c->output || !c->crtc;
    });
    restore(before);
}

// Back from another VT: the other session may have left anything on the
// CRTCs, so ours are set up again in one modeset, each with its last frame.
void Drm::restore(const std::vector<Connector*>& conns) {
    // Connectors we don't drive let go of their CRTCs (else ours can't take them).
    if (drmModeAtomicReq* req = drmModeAtomicAlloc()) {
        int n = 0;
        for (auto& c : connectors_) {
            if (std::ranges::find(conns, c.get()) != conns.end())
                continue;
            uint64_t cur = 0;
            if (!get_prop(fd_, c->id, c->props.crtc_id, &cur) || !cur)
                continue;
            drmModeAtomicAddProperty(req, c->id, c->props.crtc_id, 0);
            ++n;
            for (auto& cr : crtcs_)
                if (cr->id == cur && std::ranges::none_of(conns, [&](Connector* x) { return x->crtc == cr.get(); })) {
                    drmModeAtomicAddProperty(req, cr->id, cr->props.active, 0);
                    drmModeAtomicAddProperty(req, cr->id, cr->props.mode_id, 0);
                }
        }
        if (n && drmModeAtomicCommit(fd_, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) != 0)
            wlr_log(WLR_ERROR, "drm: %s: couldn't free the CRTCs another session left on", name_.c_str());
        drmModeAtomicFree(req);
    }
    if (conns.empty())
        return;
    const OutputState again;  // as it was: mode, last frame, cursor, gamma
    std::vector<ConnState> states;
    for (Connector* c : conns)
        states.push_back({.conn = c, .base = &again});
    if (commit_states(states, true, false, false, false))
        return;
    // One at a time; a screen that won't come back is set up anew.
    for (Connector* c : conns) {
        std::vector<ConnState> one{{.conn = c, .base = &again}};
        if (commit_states(one, true, false, false, false))
            continue;
        wlr_log(WLR_ERROR, "drm: %s: couldn't restore after the VT switch", c->name.c_str());
        const uint32_t id = c->id;
        disconnect(*c);
        scan_connectors(id);
    }
}

} // namespace atrium::backend::drm
