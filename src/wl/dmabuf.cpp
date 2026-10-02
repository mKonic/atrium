// linux-dmabuf; the parameter checks follow wlroots' types/wlr_linux_dmabuf_v1.c.
#include "wl/dmabuf.hpp"

#include "drm-server.hpp"
#include "linux-dmabuf-v1-server.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include <drm_fourcc.h>

namespace atrium::wl {

namespace {

// A dmabuf wl_buffer as the renderer sees it: its planes.
struct DmabufStorage {
    Buffer base;
    DmabufAttributes attrs;
};

const BufferImpl kDmabufImpl = {
    .destroy =
        [](Buffer* b) {
            auto* s = reinterpret_cast<DmabufStorage*>(b);
            dmabuf_attributes_finish(&s->attrs);
            buffer_finish(b);
            delete s;
        },
    .get_dmabuf =
        [](Buffer* b, DmabufAttributes* out) {
            *out = reinterpret_cast<DmabufStorage*>(b)->attrs;
            return true;
        },
    .get_shm = nullptr,
    .begin_data_ptr_access = nullptr,
    .end_data_ptr_access = nullptr,
};

class DmabufBuffer : public ClientBuffer {
public:
    DmabufBuffer(wl_client* client, uint32_t version, uint32_t id, DmabufStorage* s)
        : ClientBuffer(client, version, id, &s->base) {}
};

wl_array dev_array(dev_t* dev) {
    return wl_array{sizeof(dev_t), sizeof(dev_t), dev};
}

} // namespace

// The format table: every (format, modifier) the feedback offers, in a sealed
// memfd clients map; tranches point into it by index.
struct LinuxDmabuf::Table {
    std::vector<std::pair<uint32_t, uint64_t>> entries;
    int fd = -1;
    size_t size = 0;
    ~Table() {
        if (fd >= 0)
            close(fd);
    }
    uint16_t index_of(const std::pair<uint32_t, uint64_t>& e) const {
        return uint16_t(std::ranges::find(entries, e) - entries.begin());
    }
};

struct LinuxDmabuf::SurfaceFeedback {
    DmabufFeedback feedback;
    std::shared_ptr<Table> table;
    bool custom = false;
    std::vector<Weak<Resource>> resources;
    Connection surface_gone;
};

std::shared_ptr<LinuxDmabuf::Table> LinuxDmabuf::table_for(const DmabufFeedback& f) {
    auto t = std::make_shared<Table>();
    for (const auto& tr : f.tranches)
        for (const auto& e : tr.formats)
            if (std::ranges::find(t->entries, e) == t->entries.end())
                t->entries.push_back(e);
    struct Entry {
        uint32_t format, pad;
        uint64_t modifier;
    };
    std::vector<Entry> raw;
    for (const auto& [format, modifier] : t->entries)
        raw.push_back({format, 0, modifier});
    t->size = raw.size() * sizeof(Entry);
    t->fd = memfd_create("atrium-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (t->fd >= 0) {
        if (write(t->fd, raw.data(), t->size) != ssize_t(t->size)) {
            close(t->fd);
            t->fd = -1;
        } else {
            fcntl(t->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
        }
    }
    return t;
}

void LinuxDmabuf::send(Resource* r, const DmabufFeedback& f, const Table& table) {
    auto* fb = static_cast<ZwpLinuxDmabufFeedbackV1*>(r);
    dev_t main = f.main_device;
    wl_array a = dev_array(&main);
    fb->send_main_device(&a);
    if (table.fd >= 0)
        fb->send_format_table(table.fd, uint32_t(table.size));
    for (const auto& tr : f.tranches) {
        dev_t target = tr.target_device;
        wl_array t = dev_array(&target);
        fb->send_tranche_target_device(&t);
        std::vector<uint16_t> idx;
        for (const auto& e : tr.formats)
            idx.push_back(table.index_of(e));
        wl_array ia{idx.size() * sizeof(uint16_t), idx.size() * sizeof(uint16_t), idx.data()};
        fb->send_tranche_formats(&ia);
        fb->send_tranche_flags(tr.scanout ? uint32_t(ZwpLinuxDmabufFeedbackV1::TrancheFlags::Scanout) : 0);
        fb->send_tranche_done();
    }
    fb->send_done();
}

LinuxDmabuf::LinuxDmabuf(wl_display* display, DmabufFeedback feedback, Check check)
    : default_(std::move(feedback)), check_(std::move(check)) {
    default_table_ = table_for(default_);
    global_ = Global::create<ZwpLinuxDmabufV1>(display, 5, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* m = make<ZwpLinuxDmabufV1>(client, version, id);
        if (!m)
            return;
        m->on_create_params([this](ZwpLinuxDmabufV1* self, uint32_t id) {
            auto* p = make<ZwpLinuxBufferParamsV1>(self->client(), self->version(), id);
            if (!p)
                return;
            // The planes gathered so far; they belong to the params until used.
            auto attrs = std::make_shared<DmabufAttributes>();
            auto used = std::make_shared<bool>(false);
            p->on_gone([attrs, used] {
                if (!*used)
                    dmabuf_attributes_finish(attrs.get());
            });
            p->on_add([attrs, used](ZwpLinuxBufferParamsV1* self, int fd, uint32_t plane, uint32_t offset,
                                    uint32_t stride, uint32_t mod_hi, uint32_t mod_lo) {
                using E = ZwpLinuxBufferParamsV1::Error;
                const uint64_t modifier = (uint64_t(mod_hi) << 32) | mod_lo;
                if (*used) {
                    close(fd);
                    self->post_error(uint32_t(E::AlreadyUsed), "the params were already used");
                    return;
                }
                if (plane >= DMABUF_MAX_PLANES) {
                    close(fd);
                    self->post_error(uint32_t(E::PlaneIdx), "plane index out of range");
                    return;
                }
                if (attrs->fd[plane] != -1) {
                    close(fd);
                    self->post_error(uint32_t(E::PlaneSet), "the plane was already set");
                    return;
                }
                if (attrs->n_planes > 0 && attrs->modifier != modifier) {
                    close(fd);
                    self->post_error(uint32_t(E::InvalidFormat), "planes with different modifiers");
                    return;
                }
                attrs->modifier = modifier;
                attrs->fd[plane] = fd;
                attrs->offset[plane] = offset;
                attrs->stride[plane] = stride;
                attrs->n_planes = std::max(attrs->n_planes, int(plane) + 1);
            });
            auto create = [this, attrs, used](ZwpLinuxBufferParamsV1* self, uint32_t buffer_id, int32_t width,
                                              int32_t height, uint32_t format, uint32_t flags) {
                using E = ZwpLinuxBufferParamsV1::Error;
                if (std::exchange(*used, true)) {
                    self->post_error(uint32_t(E::AlreadyUsed), "the params were already used");
                    return;
                }
                DmabufAttributes a = *attrs;  // ours now: closed on every failure below
                auto fail = [&](bool fatal, uint32_t code, const char* message) {
                    dmabuf_attributes_finish(&a);
                    if (fatal)
                        self->post_error(code, message);
                    else if (buffer_id == 0)
                        self->send_failed();
                    else
                        self->post_error(uint32_t(E::InvalidWlBuffer), "the dmabuf couldn't be imported");
                };
                if (a.n_planes == 0 || a.fd[0] == -1)
                    return fail(true, uint32_t(E::Incomplete), "no dmabuf for plane 0");
                for (int i = 1; i < a.n_planes; ++i)
                    if (a.fd[i] == -1)
                        return fail(true, uint32_t(E::Incomplete), "a gap in the planes");
                if (flags > 7)
                    return fail(true, uint32_t(E::InvalidFormat), "unknown flags");
                if (flags != 0)
                    return fail(false, 0, "");  // y-inverted, interlaced: not supported
                if (width < 1 || height < 1)
                    return fail(true, uint32_t(E::InvalidDimensions), "a buffer needs a size");
                a.width = width;
                a.height = height;
                a.format = format;
                for (int i = 0; i < a.n_planes; ++i) {
                    if (uint64_t(a.offset[i]) + uint64_t(a.stride[i]) * uint64_t(height) > UINT32_MAX)
                        return fail(true, uint32_t(E::OutOfBounds), "the plane's size overflows");
                    const off_t size = lseek(a.fd[i], 0, SEEK_END);
                    if (size == -1)
                        continue;  // a kernel that can't seek dmabufs: can't check
                    if (a.offset[i] > size || a.offset[i] + a.stride[i] > size || a.stride[i] == 0)
                        return fail(true, uint32_t(E::OutOfBounds), "offset or stride past the dmabuf");
                    // Later planes may be subsampled.
                    if (i == 0 && a.offset[i] + uint64_t(a.stride[i]) * height > uint64_t(size))
                        return fail(true, uint32_t(E::OutOfBounds), "stride times height past the dmabuf");
                }
                if (check_ && !check_(a))
                    return fail(false, 0, "");
                auto* storage = new DmabufStorage{{}, a};
                buffer_init(&storage->base, &kDmabufImpl, width, height);
                auto* buffer = make<DmabufBuffer>(self->client(), 1, buffer_id, storage);
                if (!buffer) {
                    buffer_drop(&storage->base);
                    return;
                }
                if (buffer_id == 0)
                    self->send_created(buffer->resource());
            };
            p->on_create([create](ZwpLinuxBufferParamsV1* self, int32_t w, int32_t h, uint32_t format,
                                  uint32_t flags) { create(self, 0, w, h, format, flags); });
            p->on_create_immed([create](ZwpLinuxBufferParamsV1* self, uint32_t id, int32_t w, int32_t h,
                                        uint32_t format, uint32_t flags) { create(self, id, w, h, format, flags); });
        });
        m->on_get_default_feedback([this](ZwpLinuxDmabufV1* self, uint32_t id) {
            auto* f = make<ZwpLinuxDmabufFeedbackV1>(self->client(), self->version(), id);
            if (!f)
                return;
            std::erase_if(default_feedbacks_, [](const auto& w) { return !w; });
            default_feedbacks_.push_back(f);
            send(f, default_, *default_table_);
        });
        m->on_get_surface_feedback([this](ZwpLinuxDmabufV1* self, uint32_t id, wl_resource* surface_res) {
            auto* f = make<ZwpLinuxDmabufFeedbackV1>(self->client(), self->version(), id);
            Surface* s = Surface::from(surface_res);
            if (!f)
                return;
            if (!s) {
                f->detach();
                return;
            }
            auto& sf = surfaces_[s];
            if (!sf) {
                sf = std::make_unique<SurfaceFeedback>();
                sf->surface_gone = s->events.destroy.connect([this, s] { surfaces_.erase(s); });
            }
            std::erase_if(sf->resources, [](const auto& w) { return !w; });
            sf->resources.push_back(f);
            if (sf->custom)
                send(f, sf->feedback, *sf->table);
            else
                send(f, default_, *default_table_);
        });
        std::erase_if(managers_, [](const auto& w) { return !w; });
        managers_.push_back(m);
        // Before feedback (v4), the formats are announced on bind.
        if (version < 4) {
            for (const auto& tr : default_.tranches)
                for (const auto& [format, modifier] : tr.formats) {
                    if (version >= 3)
                        m->send_modifier(format, uint32_t(modifier >> 32), uint32_t(modifier & 0xffffffff));
                    else if (modifier == 0)  // DRM_FORMAT_MOD_LINEAR
                        m->send_format(format);
                }
        }
    });
}

LinuxDmabuf::~LinuxDmabuf() {
    global_.reset();
    for (auto* list : {&managers_, &default_feedbacks_})
        for (auto& w : *list)
            if (w)
                w->detach();
    for (auto& [s, sf] : surfaces_)
        for (auto& w : sf->resources)
            if (w)
                w->detach();
}

void LinuxDmabuf::set_surface_feedback(Surface* surface, std::optional<DmabufFeedback> feedback) {
    auto it = surfaces_.find(surface);
    if (it == surfaces_.end())
        return;  // it never asked
    SurfaceFeedback& sf = *it->second;
    sf.custom = feedback.has_value();
    if (feedback) {
        sf.feedback = std::move(*feedback);
        sf.table = table_for(sf.feedback);
    }
    for (auto& w : sf.resources)
        if (Resource* r = w.get())
            send(r, sf.custom ? sf.feedback : default_, sf.custom ? *sf.table : *default_table_);
}

LegacyDrm::LegacyDrm(wl_display* display, std::string node, std::vector<uint32_t> formats, LinuxDmabuf::Check check)
    : node_(std::move(node)), formats_(std::move(formats)), check_(std::move(check)) {
    global_ = Global::create<WlDrm>(display, 2, [this](wl_client* client, uint32_t version, uint32_t id) {
        auto* d = make<WlDrm>(client, version, id);
        if (!d)
            return;
        d->on_authenticate([](WlDrm* self, uint32_t) { self->send_authenticated(); });
        auto refuse = [](WlDrm* self) {
            self->post_error(uint32_t(WlDrm::Error::InvalidName), "only PRIME buffers are supported");
        };
        d->on_create_buffer([refuse](WlDrm* self, uint32_t, uint32_t, int32_t, int32_t, uint32_t,
                                     uint32_t) { refuse(self); });
        d->on_create_planar_buffer([refuse](WlDrm* self, uint32_t, uint32_t, int32_t, int32_t, uint32_t,
                                            int32_t, int32_t, int32_t, int32_t, int32_t,
                                            int32_t) { refuse(self); });
        d->on_create_prime_buffer([this](WlDrm* self, uint32_t id, int fd, int32_t width, int32_t height,
                                         uint32_t format, int32_t offset0, int32_t stride0, int32_t, int32_t,
                                         int32_t, int32_t) {
            DmabufAttributes a{};
            a.width = width;
            a.height = height;
            a.format = format;
            a.modifier = DRM_FORMAT_MOD_INVALID;
            a.n_planes = 1;
            a.fd[0] = fd;
            a.offset[0] = uint32_t(offset0);
            a.stride[0] = uint32_t(stride0);
            for (int i = 1; i < DMABUF_MAX_PLANES; ++i)
                a.fd[i] = -1;
            if (width < 1 || height < 1 || (check_ && !check_(a))) {
                dmabuf_attributes_finish(&a);
                self->post_error(uint32_t(WlDrm::Error::InvalidName), "the buffer can't be imported");
                return;
            }
            auto* storage = new DmabufStorage{{}, a};
            buffer_init(&storage->base, &kDmabufImpl, width, height);
            if (!make<DmabufBuffer>(self->client(), 1, id, storage))
                buffer_drop(&storage->base);
        });
        std::erase_if(resources_, [](const auto& w) { return !w; });
        resources_.push_back(d);
        d->send_device(node_.c_str());
        for (uint32_t f : formats_)
            d->send_format(f);
        if (d->version() >= 2)
            d->send_capabilities(uint32_t(WlDrm::Capability::Prime));
    });
}

LegacyDrm::~LegacyDrm() {
    global_.reset();
    for (auto& r : resources_)
        if (r)
            r->detach();
}

bool LinuxDmabuf::is_dmabuf(Buffer* buffer) {
    return buffer && buffer->impl == &kDmabufImpl;
}

} // namespace atrium::wl
