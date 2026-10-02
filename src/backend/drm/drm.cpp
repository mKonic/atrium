#include "render/pass.hpp"
#include "backend/drm/drm.hpp"

#include "backend/drm/match.hpp"
#include "listener.hpp"
#include "render/renderer.hpp"
#include "util/timeline.hpp"
#include "wlr.hpp"

extern "C" {
#include <libdisplay-info/cvt.h>
#include <libdisplay-info/info.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/pass.h>
}

#include <drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <fcntl.h>
#include <poll.h>
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

FBox src_box_of(const OutputState& s) {
    FBox b = s.buffer_src_box;
    if (b.width == 0 && b.height == 0)
        b = {0, 0, double(s.buffer->width), double(s.buffer->height)};
    return b;
}

Box dst_box_of(const OutputState& s, int w, int h) {
    Box b = s.buffer_dst_box;
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
    FormatSet formats{};
    std::vector<std::pair<int, int>> cursor_sizes;
    // Locked while KMS may show them: the one on screen, and the next.
    Buffer* current = nullptr;
    Buffer* queued = nullptr;
    FBox src{};
    Box dst{};
    // Signalled once the buffer stops being shown (explicit sync).
    Timeline* current_release = nullptr;
    uint64_t current_point = 0;
    Timeline* queued_release = nullptr;
    uint64_t queued_point = 0;

    void clear() {
        for (Buffer** b : {&current, &queued})
            if (*b) {
                buffer_unlock(*b);
                *b = nullptr;
            }
        for (auto* t : {&current_release, &queued_release})
            if (*t) {
                timeline_unref(*t);
                *t = nullptr;
            }
    }
    ~Plane() { clear(); }
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
    uint32_t lessee = 0;  // leased out (with its CRTC) to this lessee

    // The cursor plane: what to show next, where (crtc pixels, hotspot taken off).
    bool cursor_enabled = false;
    Buffer* cursor_pending = nullptr;
    int cursor_x = 0, cursor_y = 0, hotspot_x = 0, hotspot_y = 0, cursor_w = 0, cursor_h = 0;

    // A secondary GPU's copies of the frames and the cursor.
    std::unique_ptr<Swapchain> mgpu_swapchain, mgpu_cursor;

    ~Connector() {
        if (cursor_pending)
            buffer_unlock(cursor_pending);
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
    Buffer* primary = nullptr;  // locked
    uint32_t primary_fb = 0;
    FBox src{};
    Box dst{};
    Buffer* cursor = nullptr;  // locked
    uint32_t cursor_fb = 0;
    Timeline* wait = nullptr;
    uint64_t wait_point = 0;
    uint32_t mode_id = 0, gamma_lut = 0, damage_clips = 0, hdr_metadata = 0;
    int in_fence = -1;
    bool vrr = false;
    bool copied = false;  // from the parent GPU: its release is signalled already
    uint64_t colorspace = 0;

    void finish() {
        for (Buffer** b : {&primary, &cursor})
            if (*b) {
                buffer_unlock(*b);
                *b = nullptr;
            }
        if (wait) {
            timeline_unref(wait);
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
    // On a secondary GPU: what it can copy from (the parent renders).
    const FormatSet* primary_formats(uint32_t) const override {
        if (!drm.alloc_crtc(conn))
            return nullptr;
        return drm.parent_ ? &drm.mgpu_formats_ : &conn.crtc->primary->formats;
    }
    bool direct_scanout_allowed() const override { return true; }

    bool has_cursor_plane() const override { return drm.alloc_crtc(conn) && conn.crtc->cursor; }
    std::vector<std::pair<int, int>> cursor_sizes() const override {
        return has_cursor_plane() ? conn.crtc->cursor->cursor_sizes : std::vector<std::pair<int, int>>{};
    }
    const FormatSet* cursor_formats(uint32_t) const override {
        if (!has_cursor_plane())
            return nullptr;
        return drm.parent_ ? &drm.mgpu_formats_ : &conn.crtc->cursor->formats;
    }

    bool set_cursor(Buffer* buffer, int hx, int hy) override {
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
            buffer_unlock(conn.cursor_pending);
            conn.cursor_pending = nullptr;
        }
        if (buffer) {
            const bool fits = std::ranges::any_of(plane->cursor_sizes, [&](auto s) {
                return s.first == buffer->width && s.second == buffer->height;
            });
            if (!fits)
                return false;
            Buffer* shown = drm.parent_
                                    ? drm.copy_in(buffer, conn.mgpu_cursor, &plane->formats, renderer, nullptr, 0, nullptr)
                                    : buffer_lock(buffer);
            if (!shown || !drm.fb_for(shown, &plane->formats)) {
                if (shown)
                    buffer_unlock(shown);
                return false;
            }
            conn.cursor_pending = shown;
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
        Box b{x, y, 0, 0};
        box_transform(&b, &b, output_transform_invert(transform), w, h);
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

std::unique_ptr<Drm> Drm::create(wl_event_loop* loop, Session& session, const std::string& path, Drm* parent) {
    std::unique_ptr<Drm> d(new Drm(loop, session));
    d->parent_ = parent;
    d->device_ = session.open(path);
    if (!d->device_)
        return nullptr;
    d->fd_ = d->device_->fd;
    if (!drmIsKMS(d->fd_)) {
        alog(Log::Info, "drm: %s has no display outputs", path.c_str());
        return nullptr;
    }
    drmVersion* v = drmGetVersion(d->fd_);
    d->name_ = path + (v ? std::string(" (") + v->name + ")" : "");
    if (v)
        drmFreeVersion(v);
    if (!d->check_features() || !d->init_resources())
        return nullptr;
    if (parent && !d->init_mgpu())
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
                alog(Log::Error, "drm: drmHandleEvent failed");
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
    d->connections_.push_back(d->device_->events.remove.connect([raw = d.get()] { raw->removed.emit(); }));
    d->connections_.push_back(d->device_->events.change.connect([raw = d.get()](const Session::Device::Change& c) {
        if (c.type == Session::Device::Change::Type::Lease)
            raw->check_leases();
    }));
    alog(Log::Info, "drm: driving %s", d->name_.c_str());
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
    if (mgpu_timeline_)
        timeline_unref(mgpu_timeline_);
    mgpu_formats_.clear();
    mgpu_allocator_.reset();
    mgpu_dumb_.reset();
    if (mgpu_renderer_)
        mgpu_renderer_->destroy();
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
        alog(Log::Error, "drm: %s can't import buffers (PRIME)", name_.c_str());
        return false;
    }
    if (drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        alog(Log::Error, "drm: %s lacks universal planes", name_.c_str());
        return false;
    }
    if (drmGetCap(fd_, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) || !cap) {
        alog(Log::Error, "drm: %s lacks monotonic timestamps", name_.c_str());
        return false;
    }
    const char* no_atomic = std::getenv("ATRIUM_DRM_NO_ATOMIC");
    if (!no_atomic)
        no_atomic = std::getenv("WLR_DRM_NO_ATOMIC");
    atomic_ = !(no_atomic && std::string_view(no_atomic) == "1") && drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
    if (!atomic_)
        alog(Log::Info, "drm: %s: legacy modesetting (no atomic)", name_.c_str());
    if (atomic_) {
        // Virtual GPUs place the pointer by its hotspot (legacy gives it per cursor).
        drmSetClientCap(fd_, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1);
        tearing_ = drmGetCap(fd_, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
        // In-fences need atomic.
        timeline_ = drmGetCap(fd_, DRM_CAP_SYNCOBJ_TIMELINE, &cap) == 0 && cap == 1;
    } else {
        tearing_ = drmGetCap(fd_, DRM_CAP_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
    }
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
            p->formats.add(info->formats[f], DRM_FORMAT_MOD_LINEAR);
            if (p->type != DRM_PLANE_TYPE_CURSOR)
                p->formats.add(info->formats[f], DRM_FORMAT_MOD_INVALID);
        }
        uint64_t blob_id = 0;
        if (p->props.in_formats && addfb2_modifiers_ && get_prop(fd_, p->id, p->props.in_formats, &blob_id) && blob_id) {
            if (drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd_, uint32_t(blob_id))) {
                drmModeFormatModifierIterator it{};
                while (drmModeFormatModifierBlobIterNext(blob, &it))
                    p->formats.add(it.fmt, it.mod);
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
    alog(Log::Info, "drm: %zu CRTCs, %zu planes", crtcs_.size(), planes_.size());
    return !crtcs_.empty();
}

uint32_t Drm::buffer_caps() const {
    return BUFFER_CAP_DMABUF;
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
            const uint32_t current = current_crtc(cid, info);
            for (auto& cr : crtcs_)
                if (current && cr->id == current)
                    made->crtc = cr.get();
            c = made.get();
            connectors_.push_back(std::move(made));
            alog(Log::Info, "drm: found connector %s", c->name.c_str());
        }
        seen.push_back(c);
        // A link gone bad (a DP cable wiggled): its modes are read again and
        // it is set up anew.
        uint64_t link = 0;
        if (c->props.link_status && get_prop(fd_, cid, c->props.link_status, &link) &&
            link == DRM_MODE_LINK_STATUS_BAD && c->output) {
            alog(Log::Info, "drm: %s: bad link", c->name.c_str());
            disconnect(*c);
        }
        if (!c->output && info->connection == DRM_MODE_CONNECTED) {
            alog(Log::Info, "drm: %s connected", c->name.c_str());
            if (connect(*c, *info))
                fresh.push_back(c);
        } else if (c->output && info->connection != DRM_MODE_CONNECTED) {
            alog(Log::Info, "drm: %s disconnected", c->name.c_str());
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
    if (c.crtc && !atomic_) {
        if (drmModeCrtc* cr = drmModeGetCrtc(fd_, c.crtc->id)) {
            if (cr->mode_valid) {
                current = cr->mode;
                has_current = true;
            }
            drmModeFreeCrtc(cr);
        }
    } else if (c.crtc) {
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
    if (c.lessee) {
        // Leased out: the lease ends with it; its CRTC was never ours to turn off.
        const uint32_t l = c.lessee;
        end_lease(l, true);
        lease_ended.emit(l);
    }
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
        buffer_unlock(c.cursor_pending);
        c.cursor_pending = nullptr;
    }
    destroy_blob(fd_, c.hdr_metadata);
    c.hdr_metadata = 0;
    c.mgpu_swapchain.reset();
    c.mgpu_cursor.reset();
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
        // Only for those on (or leased out), or the one about to be.
        const bool wants = &c == want || (c.output && (c.output->enabled || c.lessee));
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
    // A screen that's on (or leased) keeps its CRTC, or nothing changes.
    for (size_t i = 0; i < connectors_.size(); ++i) {
        Connector& c = *connectors_[i];
        if (&c == want || !c.output || !(c.output->enabled || c.lessee))
            continue;  // (the one asking has none yet)
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

uint32_t Drm::fb_for(Buffer* buffer, const FormatSet* formats) {
    if (auto it = fbs_.find(buffer); it != fbs_.end())
        return it->second->poisoned ? 0 : it->second->id;
    DmabufAttributes a;
    if (!buffer_get_dmabuf(buffer, &a))
        return 0;
    if (formats && !formats->has(a.format, a.modifier))
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
        alog(Log::Debug, "drm: buffer 0x%x/0x%lx refused for scan-out", a.format, (unsigned long)a.modifier);
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
        alog(Log::Debug, "drm: %s: no CRTC free", c.name.c_str());
        return false;
    }
    if (!on && !c.crtc)
        return true;  // off already
    const bool modeset = state.allow_reconfiguration;
    // A flip without a modeset waits for the previous one.
    const bool nonblock = !modeset && (state.committed & OutputState::Buffer);
    if (!test_only && nonblock && c.pending_flip) {
        alog(Log::Debug, "drm: %s: a page flip is still pending", c.name.c_str());
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
bool Drm::prepare(ConnState& st, bool modeset, bool test_only) {
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
        const bool wait = (s.committed & OutputState::WaitTimeline) && s.wait_timeline;
        Buffer* last = primary.queued ? primary.queued : primary.current;
        if ((s.committed & OutputState::Buffer) && parent_ && test_only && last &&
            last->width == s.buffer->width && last->height == s.buffer->height) {
            // A test needn't copy: the last copy stands in.
            st.primary_fb = fb_for(last, &primary.formats);
            if (!st.primary_fb)
                return false;
            st.primary = buffer_lock(last);
            st.src = src_box_of(s);
            st.dst = dst_box_of(s, last->width, last->height);
        } else if ((s.committed & OutputState::Buffer) && parent_) {
            // The parent GPU drew it: copied into one of ours first. The copy
            // waits for the frame, KMS for the copy, and the frame is free
            // once copied.
            int fence = -1;
            st.primary = copy_in(s.buffer, c.mgpu_swapchain, &primary.formats, o.renderer,
                                 wait ? s.wait_timeline : nullptr, s.wait_point, &fence);
            if (!st.primary)
                return false;
            st.primary_fb = fb_for(st.primary, &primary.formats);
            if (!st.primary_fb) {
                if (fence >= 0)
                    close(fence);
                return false;
            }
            st.src = src_box_of(s);
            st.dst = dst_box_of(s, s.buffer->width, s.buffer->height);
            st.in_fence = fence;
            st.copied = true;
            if (!test_only && (s.committed & OutputState::SignalTimeline) && s.signal_timeline) {
                if (fence >= 0)
                    timeline_import_sync_file(s.signal_timeline, s.signal_point, fence);
                else
                    timeline_signal(s.signal_timeline, s.signal_point);  // copied already
            }
        } else if (s.committed & OutputState::Buffer) {
            st.primary_fb = fb_for(s.buffer, &primary.formats);
            if (!st.primary_fb)
                return false;
            st.primary = buffer_lock(s.buffer);
            st.src = src_box_of(s);
            st.dst = dst_box_of(s, s.buffer->width, s.buffer->height);
            if (wait) {
                st.wait = timeline_ref(s.wait_timeline);
                st.wait_point = s.wait_point;
            }
        } else if (Buffer* b = primary.queued ? primary.queued : primary.current) {
            st.primary_fb = fb_for(b, &primary.formats);
            st.primary = buffer_lock(b);
            st.src = primary.src;
            st.dst = primary.dst;
        }
        if (!st.primary_fb)
            return false;  // nothing to show
        if (c.cursor_enabled && crtc.cursor) {
            Buffer* cb = c.cursor_pending ? c.cursor_pending
                             : crtc.cursor->queued ? crtc.cursor->queued
                                                   : crtc.cursor->current;
            if (cb) {
                st.cursor_fb = fb_for(cb, &crtc.cursor->formats);
                if (st.cursor_fb)
                    st.cursor = buffer_lock(cb);
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
    if ((s.committed & OutputState::ColorTransform) && !atomic_) {
        if (s.color_transform && crtc->legacy_gamma_size < 2)
            return false;  // set at commit: legacy has no blobs for it
    } else if (s.committed & OutputState::ColorTransform) {
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
        st.in_fence = timeline_export_sync_file(st.wait, st.wait_point);
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
        if (d && !atomic_)
            return false;  // HDR signalling is atomic-only here
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
        if (!prepare(st, modeset, test_only)) {
            ok = false;
            break;
        }
    }

    drmModeAtomicReq* req = ok && atomic_ ? drmModeAtomicAlloc() : nullptr;
    auto add = [&](uint32_t obj, uint32_t prop, uint64_t value) {
        if (ok && prop && drmModeAtomicAddProperty(req, obj, prop, value) < 0)
            ok = false;
    };
    auto plane_off = [&](Plane& p) {
        add(p.id, p.props.fb_id, 0);
        add(p.id, p.props.crtc_id, 0);
    };
    auto plane_on = [&](Plane& p, uint32_t fb, uint32_t crtc, const Box& dst, const FBox& src) {
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
    if (ok && atomic_)
        for (ConnState& st : states) {
            Connector& c = *st.conn;
            Crtc& crtc = *c.crtc;
            add(c.id, c.props.crtc_id, st.active ? crtc.id : 0);
            if (modeset && st.active && c.props.link_status)
                add(c.id, c.props.link_status, DRM_MODE_LINK_STATUS_GOOD);
            if (st.active && c.props.content_type)
                add(c.id, c.props.content_type, DRM_MODE_CONTENT_TYPE_GRAPHICS);
            if (modeset && st.active && c.props.max_bpc && c.max_bpc_max && st.primary) {
                DmabufAttributes a;
                uint64_t bpc = max_bpc_for(buffer_get_dmabuf(st.primary, &a) ? a.format : 0);
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
                        const FBox src{0, 0, double(st.cursor->width), double(st.cursor->height)};
                        const Box dst{c.cursor_x, c.cursor_y, st.cursor->width, st.cursor->height};
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
    if (ok && !atomic_) {
        ok = legacy_commit(states, modeset, test_only, async, flip);
    } else if (ok && drmModeAtomicCommit(fd_, req, flags, flip) != 0) {
        alog(test_only ? Log::Debug : Log::Error, "drm: atomic commit (%s%s) failed: %s",
                states.size() == 1 ? states[0].conn->name.c_str() : "several screens",
                modeset ? ", modeset" : "", std::strerror(errno));
        ok = false;
    }
    if (!ok && flip && atomic_)
        flip->conns.clear();  // nothing will answer
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
                    buffer_unlock(primary.queued);
                primary.queued = std::exchange(st.primary, nullptr);
                primary.src = st.src;
                primary.dst = st.dst;
            }
            if (primary.queued_release) {
                // Replaced before it was ever shown.
                timeline_signal(primary.queued_release, primary.queued_point);
                timeline_unref(primary.queued_release);
                primary.queued_release = nullptr;
            }
            if ((st.base->committed & OutputState::SignalTimeline) && st.base->signal_timeline && !st.copied) {
                primary.queued_release = timeline_ref(st.base->signal_timeline);
                primary.queued_point = st.base->signal_point;
            }
            if (crtc.cursor) {
                if (crtc.cursor->queued)
                    buffer_unlock(crtc.cursor->queued);
                crtc.cursor->queued = std::exchange(st.cursor, nullptr);
            }
            if (c.cursor_pending) {
                buffer_unlock(c.cursor_pending);
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
        // (A legacy commit that failed part way still has flips in flight.)
        if (!flip->conns.empty())
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
            buffer_unlock(primary.current);
        primary.current = std::exchange(primary.queued, nullptr);
        if (primary.current_release) {
            // No longer on screen: the client may reuse it.
            timeline_signal(primary.current_release, primary.current_point);
            timeline_unref(primary.current_release);
        }
        primary.current_release = std::exchange(primary.queued_release, nullptr);
        primary.current_point = primary.queued_point;
    }
    if (Plane* cur = c->crtc->cursor; cur && cur->queued) {
        if (cur->current)
            buffer_unlock(cur->current);
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

bool Drm::supports_timelines() const {
    if (parent_)
        return timeline_ && mgpu_timeline_;
    return timeline_;
}

// A secondary GPU: a renderer of its own to copy the parent's frames with.
bool Drm::init_mgpu() {
    // Copies are cheap enough on the CPU where the GPU has no 3D (DisplayLink).
    mgpu_renderer_ = render::Renderer::create_on(fd_, true);
    if (!mgpu_renderer_) {
        alog(Log::Error, "drm: %s: no renderer to copy frames to it with", name_.c_str());
        return false;
    }
    mgpu_allocator_ = Allocator::create(fd_);
    if (!mgpu_allocator_) {
        alog(Log::Error, "drm: %s: no allocator for copies", name_.c_str());
        return false;
    }
    // What it reads of another GPU's buffers. Implicit modifiers mean
    // something different on each GPU, so only explicit ones.
    const FormatSet* tex = mgpu_renderer_->texture_formats(BUFFER_CAP_DMABUF);
    if (tex)
        for (const DrmFormat& f : *tex)
            for (uint64_t m : f.modifiers)
                if (m != DRM_FORMAT_MOD_INVALID)
                    mgpu_formats_.add(f.format, m);
    if (mgpu_formats_.empty()) {
        alog(Log::Error, "drm: %s can't read other GPUs' buffers", name_.c_str());
        return false;
    }
    if (timeline_ && mgpu_renderer_->features.timeline)
        mgpu_timeline_ = timeline_create(fd_);
    alog(Log::Info, "drm: %s shows frames rendered on %s", name_.c_str(), parent_->name_.c_str());
    return true;
}

// Through the CPU: `from` (the parent's renderer) reads the frame back into
// our buffer's mapping. Waits for the frame on the CPU first.
bool Drm::cpu_copy(Buffer* src, Buffer* dst, render::Renderer* from, Timeline* wait,
                   uint64_t wait_point) {
    if (!from)
        return false;
    if (wait) {
        const int fd = timeline_export_sync_file(wait, wait_point);
        if (fd >= 0) {
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 1000);
            close(fd);
        }
    }
    render::Texture* tex = from->texture_from_buffer(src);
    if (!tex) {
        alog(Log::Error, "drm: %s: the frame can't be read back", name_.c_str());
        return false;
    }
    void* data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    bool ok = false;
    if (buffer_begin_data_ptr_access(dst, BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        render::ReadPixelsOptions o{};
        o.data = data;
        o.format = format;
        o.stride = uint32_t(stride);
        ok = tex->read_pixels(&o);
        buffer_end_data_ptr_access(dst);
    } else {
        alog(Log::Error, "drm: %s: couldn't map a buffer to copy into", name_.c_str());
    }
    tex->destroy();
    return ok;
}

Buffer* Drm::copy_in(Buffer* src, std::unique_ptr<Swapchain>& sc, const FormatSet* formats,
                         render::Renderer* from, Timeline* wait, uint64_t wait_point, int* fence) {
    if (fence)
        *fence = -1;
    DmabufAttributes a;
    if (!buffer_get_dmabuf(src, &a))
        return nullptr;
    render::Texture* tex = mgpu_cpu_ ? nullptr : mgpu_renderer_->texture_from_buffer(src);
    if (!tex && !mgpu_cpu_) {
        // It can't read the parent's buffers (they live in the other GPU's
        // memory): the parent reads them back and the CPU writes ours.
        mgpu_dumb_ = Allocator::create_dumb(fd_);
        if (!mgpu_dumb_) {
            alog(Log::Error, "drm: %s can't read the frames and has no dumb buffers", name_.c_str());
            return nullptr;
        }
        alog(Log::Info, "drm: %s: copying frames through the CPU", name_.c_str());
        mgpu_cpu_ = true;
        for (auto& c : connectors_) {
            c->mgpu_swapchain.reset();
            c->mgpu_cursor.reset();
        }
    }
    if (!sc || sc->width != src->width || sc->height != src->height || sc->format != a.format) {
        sc.reset();
        // What the plane shows and this GPU draws into (a CPU writes linear).
        const DrmFormat* shown = formats->get(a.format);
        const DrmFormat* drawn = mgpu_renderer_->egl().render_formats()->get(a.format);
        std::vector<uint64_t> mods;
        if (shown)
            for (uint64_t m : shown->modifiers)
                if (mgpu_cpu_ ? m == DRM_FORMAT_MOD_LINEAR : drawn && drawn->has(m))
                    mods.push_back(m);
        if (mods.empty()) {
            alog(Log::Error, "drm: %s: no buffer for copies of 0x%08x", name_.c_str(), a.format);
            if (tex)
                tex->destroy();
            return nullptr;
        }
        sc = std::make_unique<Swapchain>(mgpu_cpu_ ? *mgpu_dumb_ : *mgpu_allocator_, src->width, src->height,
                                         a.format, mods);
    }
    Buffer* dst = sc->acquire();
    if (!dst) {
        alog(Log::Error, "drm: %s: couldn't allocate a buffer to copy into", name_.c_str());
        sc.reset();
        if (tex)
            tex->destroy();
        return nullptr;
    }
    if (mgpu_cpu_) {
        const bool ok = cpu_copy(src, dst, from, wait, wait_point);
        if (!ok)
            buffer_unlock(dst);
        return ok ? dst : nullptr;
    }
    render::BufferPassOptions opts{};
    const bool signal = mgpu_timeline_ && fence;
    if (signal) {
        opts.signal_timeline = mgpu_timeline_;
        opts.signal_point = ++mgpu_point_;
    }
    bool ok = false;
    if (render::RenderPass* pass = mgpu_renderer_->begin_buffer_pass(dst, &opts)) {
        render::TextureOptions t{};
        t.texture = tex;
        t.blend_mode = render::BLEND_MODE_NONE;
        t.wait_timeline = mgpu_timeline_ ? wait : nullptr;
        t.wait_point = wait_point;
        pass->add_texture(&t);
        ok = pass->submit();
    }
    tex->destroy();
    if (!ok) {
        buffer_unlock(dst);
        return nullptr;
    }
    if (signal)
        *fence = timeline_export_sync_file(mgpu_timeline_, opts.signal_point);
    return dst;
}

// ---- leases ----------------------------------------------------------------------------

uint32_t Drm::connector_id(const Output* o) const {
    for (const auto& c : connectors_)
        if (c->output == o)
            return c->id;
    return 0;
}

// For clients to look at the device: the same node, without DRM master.
int Drm::non_master_fd() const {
    char* name = drmGetDeviceNameFromFd2(fd_);
    if (!name)
        return -1;
    const int fd = open(name, O_RDWR | O_CLOEXEC);
    std::free(name);
    if (fd >= 0 && drmIsMaster(fd) && drmDropMaster(fd) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Each connector goes with a CRTC and its primary plane (what a VR runtime
// needs to show anything).
int Drm::create_lease(const std::vector<uint32_t>& connector_ids, uint32_t* lessee) {
    std::vector<Connector*> conns;
    for (uint32_t id : connector_ids) {
        auto it = std::ranges::find_if(connectors_, [id](const auto& c) { return c->id == id; });
        if (it == connectors_.end() || !(*it)->output || (*it)->lessee || (*it)->output->enabled)
            return -1;
        conns.push_back(it->get());
    }
    std::vector<uint32_t> objects;
    for (Connector* c : conns) {
        c->lessee = ~0u;  // wants a CRTC as if on
        if (!alloc_crtc(*c)) {
            for (Connector* x : conns)
                if (x->lessee == ~0u) {
                    x->lessee = 0;
                    x->crtc = nullptr;
                }
            alog(Log::Error, "drm: %s: no CRTC to lease with %s", name_.c_str(), c->name.c_str());
            return -1;
        }
        objects.push_back(c->id);
        objects.push_back(c->crtc->id);
        objects.push_back(c->crtc->primary->id);
    }
    const int fd = drmModeCreateLease(fd_, objects.data(), int(objects.size()), O_CLOEXEC, lessee);
    for (Connector* c : conns) {
        c->lessee = fd >= 0 ? *lessee : 0;
        if (fd < 0)
            c->crtc = nullptr;
    }
    if (fd < 0)
        alog(Log::Error, "drm: %s: creating a lease failed: %s", name_.c_str(), std::strerror(errno));
    else
        alog(Log::Info, "drm: %s: lease %u granted", name_.c_str(), *lessee);
    return fd;
}

void Drm::end_lease(uint32_t lessee, bool revoke) {
    if (revoke && drmModeRevokeLease(fd_, lessee) != 0)
        alog(Log::Debug, "drm: %s: revoking lease %u: %s", name_.c_str(), lessee, std::strerror(errno));
    for (auto& c : connectors_)
        if (c->lessee == lessee) {
            c->lessee = 0;
            c->crtc = nullptr;
        }
}

void Drm::revoke_lease(uint32_t lessee) {
    end_lease(lessee, true);
}

// A LEASE uevent: lessees the kernel no longer lists are over.
void Drm::check_leases() {
    std::vector<uint32_t> ours;
    for (const auto& c : connectors_)
        if (c->lessee && std::ranges::find(ours, c->lessee) == ours.end())
            ours.push_back(c->lessee);
    if (ours.empty())
        return;
    drmModeLesseeListPtr list = drmModeListLessees(fd_);
    for (uint32_t l : ours) {
        bool alive = false;
        for (uint32_t i = 0; list && i < list->count; ++i)
            alive |= list->lessees[i] == l;
        if (!alive) {
            alog(Log::Info, "drm: %s: lease %u ended", name_.c_str(), l);
            end_lease(l, false);
            lease_ended.emit(l);
        }
    }
    drmFree(list);
}

// The CRTC driving a connector now (the boot splash's, another session's).
uint32_t Drm::current_crtc(uint32_t connector, const drmModeConnector* info) const {
    if (atomic_) {
        const auto it = std::ranges::find_if(connectors_, [&](const auto& c) { return c->id == connector; });
        ConnectorProps props;
        if (it != connectors_.end())
            props = (*it)->props;
        else
            get_props(fd_, connector, &props);
        uint64_t v = 0;
        return get_prop(fd_, connector, props.crtc_id, &v) ? uint32_t(v) : 0;
    }
    uint32_t crtc = 0;
    if (info && info->encoder_id)
        if (drmModeEncoder* e = drmModeGetEncoder(fd_, info->encoder_id)) {
            crtc = e->crtc_id;
            drmModeFreeEncoder(e);
        }
    return crtc;
}

// Without atomic: one call per thing, screen by screen (wlroots' legacy.c).
// Nothing can be tested ahead, so a test passes what prepare() accepted.
bool Drm::legacy_commit(std::vector<ConnState>& states, bool modeset, bool test_only, bool async, PageFlip* flip) {
    if (test_only)
        return true;
    std::vector<std::pair<Connector*, uint32_t>> sent;
    bool ok = true;
    for (ConnState& st : states) {
        Connector& c = *st.conn;
        Crtc& crtc = *c.crtc;
        if (modeset) {
            uint32_t conn_id = c.id;
            const int r = st.active ? drmModeSetCrtc(fd_, crtc.id, st.primary_fb, 0, 0, &conn_id, 1, &st.mode)
                                    : drmModeSetCrtc(fd_, crtc.id, 0, 0, 0, nullptr, 0, nullptr);
            if (r != 0) {
                alog(Log::Error, "drm: %s: modeset failed: %s", c.name.c_str(), std::strerror(errno));
                ok = false;
                break;
            }
            if (st.active && c.props.link_status)
                drmModeConnectorSetProperty(fd_, c.id, c.props.link_status, DRM_MODE_LINK_STATUS_GOOD);
        }
        if (!st.active)
            continue;
        if (crtc.props.vrr_enabled)
            drmModeObjectSetProperty(fd_, crtc.id, DRM_MODE_OBJECT_CRTC, crtc.props.vrr_enabled, st.vrr);
        if ((st.base->committed & OutputState::ColorTransform) && crtc.legacy_gamma_size > 1) {
            const size_t n = size_t(crtc.legacy_gamma_size);
            std::vector<uint16_t> r(n), g(n), b(n);
            for (size_t i = 0; i < n; ++i) {
                const float x = float(i) / float(n - 1);
                float out[3] = {x, x, x};
                if (st.base->color_transform) {
                    const float in[3] = {x, x, x};
                    wlr_color_transform_eval(st.base->color_transform, out, in);
                }
                r[i] = uint16_t(std::lround(std::clamp(out[0], 0.0f, 1.0f) * 65535));
                g[i] = uint16_t(std::lround(std::clamp(out[1], 0.0f, 1.0f) * 65535));
                b[i] = uint16_t(std::lround(std::clamp(out[2], 0.0f, 1.0f) * 65535));
            }
            if (drmModeCrtcSetGamma(fd_, crtc.id, uint32_t(n), r.data(), g.data(), b.data()) != 0)
                alog(Log::Error, "drm: %s: setting gamma failed: %s", c.name.c_str(), std::strerror(errno));
        }
        if (crtc.cursor) {
            DmabufAttributes a;
            uint32_t handle = 0;
            if (st.cursor && c.output->cursor_visible() && buffer_get_dmabuf(st.cursor, &a) &&
                drmPrimeFDToHandle(fd_, a.fd[0], &handle) == 0) {
                if (drmModeSetCursor2(fd_, crtc.id, handle, uint32_t(a.width), uint32_t(a.height), c.hotspot_x,
                                      c.hotspot_y) != 0)
                    alog(Log::Debug, "drm: %s: setting the cursor failed", c.name.c_str());
                drmModeMoveCursor(fd_, crtc.id, c.cursor_x, c.cursor_y);  // its top left
                drmCloseBufferHandle(fd_, handle);
            } else {
                drmModeSetCursor(fd_, crtc.id, 0, 0, 0);
            }
        }
        if (flip) {
            const uint32_t f = DRM_MODE_PAGE_FLIP_EVENT | (async ? DRM_MODE_PAGE_FLIP_ASYNC : 0);
            if (drmModePageFlip(fd_, crtc.id, st.primary_fb, f, flip) != 0) {
                alog(Log::Error, "drm: %s: page flip failed: %s", c.name.c_str(), std::strerror(errno));
                ok = false;
                break;
            }
            sent.emplace_back(&c, crtc.id);
        }
    }
    if (flip)
        flip->conns = std::move(sent);  // only these will answer
    return ok;
}

void Drm::session_active(bool active) {
    alog(Log::Info, "drm: %s %s", name_.c_str(), active ? "resumed" : "paused");
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
    if (drmModeAtomicReq* req = atomic_ ? drmModeAtomicAlloc() : nullptr) {
        int n = 0;
        for (auto& c : connectors_) {
            if (std::ranges::find(conns, c.get()) != conns.end() || c->lessee)
                continue;  // ours, or a lessee's
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
            alog(Log::Error, "drm: %s: couldn't free the CRTCs another session left on", name_.c_str());
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
        alog(Log::Error, "drm: %s: couldn't restore after the VT switch", c->name.c_str());
        const uint32_t id = c->id;
        disconnect(*c);
        scan_connectors(id);
    }
}

} // namespace atrium::backend::drm
