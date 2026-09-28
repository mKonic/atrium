#include "wl/shm.hpp"

#include "render/formats.hpp"

#include <drm_fourcc.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace atrium::wl {

namespace {

// One pool's memory; buffers keep it alive after the pool goes.
struct Mapping {
    void* data = MAP_FAILED;
    size_t size = 0;
    int fd = -1;
    bool bus_error = false;  // the client truncated the file under a read

    ~Mapping() {
        if (data != MAP_FAILED)
            munmap(data, size);
        if (fd >= 0)
            close(fd);
    }
};

// The mapping being read right now: a SIGBUS inside it (the file was
// shrunk) swaps in zeroed memory so the read completes, as libwayland's own
// wl_shm does.
thread_local Mapping* g_accessing = nullptr;
struct sigaction g_previous_sigbus{};

void sigbus(int sig, siginfo_t* info, void* context) {
    Mapping* m = g_accessing;
    auto* addr = static_cast<char*>(info->si_addr);
    auto* base = m ? static_cast<char*>(m->data) : nullptr;
    if (!m || addr < base || addr >= base + m->size) {
        // Not ours: whoever handled it before.
        if (g_previous_sigbus.sa_flags & SA_SIGINFO)
            g_previous_sigbus.sa_sigaction(sig, info, context);
        else if (g_previous_sigbus.sa_handler == SIG_DFL || g_previous_sigbus.sa_handler == SIG_IGN) {
            signal(SIGBUS, SIG_DFL);
            raise(SIGBUS);
        } else
            g_previous_sigbus.sa_handler(sig);
        return;
    }
    m->bus_error = true;
    // Zeroes over the whole mapping; a failure here leaves the fault to recur.
    if (mmap(m->data, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
        signal(SIGBUS, SIG_DFL);
}

void install_sigbus_handler() {
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    struct sigaction sa{};
    sa.sa_sigaction = sigbus;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g_previous_sigbus);
}

// A buffer in a pool, as the renderer sees it.
struct ShmStorage {
    wlr_buffer base;
    std::shared_ptr<Mapping> mapping;
    size_t offset;
    int stride;
    uint32_t format;  // DRM
    Weak<WlBuffer> owner;
};

ShmStorage* storage_of(wlr_buffer* b) {
    return reinterpret_cast<ShmStorage*>(b);
}

const wlr_buffer_impl kShmBufferImpl = {
    .destroy =
        [](wlr_buffer* b) {
            wlr_buffer_finish(b);
            delete storage_of(b);
        },
    .get_dmabuf = nullptr,
    .get_shm =
        [](wlr_buffer* b, wlr_shm_attributes* out) {
            ShmStorage* s = storage_of(b);
            *out = {.fd = s->mapping->fd, .format = s->format, .width = b->width, .height = b->height,
                    .stride = s->stride, .offset = off_t(s->offset)};
            return true;
        },
    .begin_data_ptr_access =
        [](wlr_buffer* b, uint32_t flags, void** data, uint32_t* format, size_t* stride) {
            if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
                return false;  // the client's memory: read only
            ShmStorage* s = storage_of(b);
            g_accessing = s->mapping.get();
            *data = static_cast<char*>(s->mapping->data) + s->offset;
            *format = s->format;
            *stride = size_t(s->stride);
            return true;
        },
    .end_data_ptr_access =
        [](wlr_buffer* b) {
            ShmStorage* s = storage_of(b);
            g_accessing = nullptr;
            if (s->mapping->bus_error)
                if (WlBuffer* o = s->owner.get()) {
                    wl_resource_post_error(o->resource(), uint32_t(WlShm::Error::InvalidFd),
                                           "the shm file was shrunk under a buffer");
                }
        },
};

class ShmBuffer : public ClientBuffer {
public:
    ShmBuffer(wl_client* client, uint32_t version, uint32_t id, ShmStorage* storage)
        : ClientBuffer(client, version, id, &storage->base) {
        storage->owner = this;
    }
};

class ShmPool : public WlShmPool {
public:
    ShmPool(wl_client* client, uint32_t version, uint32_t id, std::shared_ptr<const std::vector<uint32_t>> formats,
            std::shared_ptr<Mapping> mapping)
        : WlShmPool(client, version, id), formats_(std::move(formats)), mapping_(std::move(mapping)) {
        on_create_buffer([this](WlShmPool*, uint32_t id, int32_t offset, int32_t width, int32_t height,
                                int32_t stride, uint32_t format) {
            create_buffer(id, offset, width, height, stride, format);
        });
        on_resize([this](WlShmPool*, int32_t size) { resize(size); });
    }

private:
    void create_buffer(uint32_t id, int32_t offset, int32_t width, int32_t height, int32_t stride,
                       uint32_t wl_format) {
        const uint32_t format = Shm::to_drm(wl_format);
        const render::PixelFormat* pf = render::format_from_drm(format);
        if (!pf || std::ranges::find(*formats_, format) == formats_->end()) {
            post_error(uint32_t(WlShm::Error::InvalidFormat), "unsupported format");
            return;
        }
        if (offset < 0 || width <= 0 || height <= 0 || stride < width * pf->bytes_per_pixel ||
            size_t(offset) + size_t(stride) * size_t(height) > mapping_->size) {
            post_error(uint32_t(WlShm::Error::InvalidStride), "the buffer doesn't fit its pool");
            return;
        }
        auto* storage = new ShmStorage{{}, mapping_, size_t(offset), stride, format, {}};
        wlr_buffer_init(&storage->base, &kShmBufferImpl, width, height);
        if (!make<ShmBuffer>(client(), 1, id, storage))
            wlr_buffer_drop(&storage->base);
    }

    void resize(int32_t size) {
        if (size <= 0 || size_t(size) < mapping_->size) {
            post_error(uint32_t(WlShm::Error::InvalidStride), "a pool can only grow");
            return;
        }
        void* data = mremap(mapping_->data, mapping_->size, size_t(size), MREMAP_MAYMOVE);
        if (data == MAP_FAILED) {
            post_error(uint32_t(WlShm::Error::InvalidFd), "can't map the grown pool");
            return;
        }
        mapping_->data = data;
        mapping_->size = size_t(size);
    }

    std::shared_ptr<const std::vector<uint32_t>> formats_;
    std::shared_ptr<Mapping> mapping_;
};

} // namespace

uint32_t Shm::to_drm(uint32_t wl_format) {
    switch (wl_format) {
    case uint32_t(WlShm::Format::Argb8888):
        return DRM_FORMAT_ARGB8888;
    case uint32_t(WlShm::Format::Xrgb8888):
        return DRM_FORMAT_XRGB8888;
    default:
        return wl_format;
    }
}

uint32_t Shm::from_drm(uint32_t drm_format) {
    switch (drm_format) {
    case DRM_FORMAT_ARGB8888:
        return uint32_t(WlShm::Format::Argb8888);
    case DRM_FORMAT_XRGB8888:
        return uint32_t(WlShm::Format::Xrgb8888);
    default:
        return drm_format;
    }
}

Shm::Shm(wl_display* display, std::vector<uint32_t> formats) {
    for (uint32_t f : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888})
        if (std::ranges::find(formats, f) == formats.end())
            formats.push_back(f);
    formats_ = std::make_shared<const std::vector<uint32_t>>(std::move(formats));
    install_sigbus_handler();
    global_ = Global::create<WlShm>(display, 2, [this](wl_client* client, uint32_t version,
                                                                             uint32_t id) {
        auto* shm = make<WlShm>(client, version, id);
        if (!shm)
            return;
        shm->on_create_pool([this](WlShm* self, uint32_t id, int fd, int32_t size) {
            auto mapping = std::make_shared<Mapping>();
            mapping->fd = fd;
            if (size <= 0) {
                self->post_error(uint32_t(WlShm::Error::InvalidStride), "a pool needs a size");
                return;
            }
            mapping->data = mmap(nullptr, size_t(size), PROT_READ, MAP_SHARED, fd, 0);
            if (mapping->data == MAP_FAILED) {
                self->post_error(uint32_t(WlShm::Error::InvalidFd), "can't map the pool");
                return;
            }
            mapping->size = size_t(size);
            make<ShmPool>(self->client(), self->version(), id, formats_, std::move(mapping));
        });
        std::erase_if(shms_, [](const auto& w) { return !w; });
        shms_.push_back(shm);
        for (uint32_t f : *formats_)
            shm->send_format(from_drm(f));
    });
}

Shm::~Shm() {
    global_.reset();
    for (auto& s : shms_)
        if (s)
            s->detach();
}

bool Shm::supports(uint32_t drm_format) const {
    return std::ranges::find(*formats_, drm_format) != formats_->end();
}

} // namespace atrium::wl
