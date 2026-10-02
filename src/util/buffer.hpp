#pragma once
// A buffer of pixels with locks: what clients attach, swapchains hand out
// and screens show. Whoever made it drops it; it goes once dropped and
// unlocked. After wlroots' types/buffer/buffer.c and util/addon.c (MIT),
// whose shape atrium's code was written against.
#include <wayland-server-core.h>

#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace atrium {

constexpr int DMABUF_MAX_PLANES = 4;

struct DmabufAttributes {
    int32_t width = 0, height = 0;
    uint32_t format = 0;  // DRM_FORMAT_*
    uint64_t modifier = 0;
    int n_planes = 0;
    uint32_t offset[DMABUF_MAX_PLANES] = {};
    uint32_t stride[DMABUF_MAX_PLANES] = {};
    int fd[DMABUF_MAX_PLANES] = {-1, -1, -1, -1};
};
// Closes its fds.
void dmabuf_attributes_finish(DmabufAttributes* attribs);
// A copy with its own (duplicated) fds.
bool dmabuf_attributes_copy(DmabufAttributes* dst, const DmabufAttributes* src);

struct ShmAttributes {
    int fd = -1;
    uint32_t format = 0;
    int width = 0, height = 0, stride = 0;
    off_t offset = 0;
};

// What a buffer can give (a backend or renderer says what it takes).
enum BufferCaps : uint32_t {
    BUFFER_CAP_DATA_PTR = 1 << 0,
    BUFFER_CAP_DMABUF = 1 << 1,
    BUFFER_CAP_SHM = 1 << 2,
};

enum BufferAccess : uint32_t {
    BUFFER_DATA_PTR_ACCESS_READ = 1 << 0,
    BUFFER_DATA_PTR_ACCESS_WRITE = 1 << 1,
};

// Per-owner extras hung on an object (a framebuffer the renderer made for
// a buffer), destroyed with it.
struct AddonSet;
struct AddonInterface {
    const char* name;
    void (*destroy)(struct Addon* addon);
};
struct Addon {
    const AddonInterface* impl = nullptr;
    const void* owner = nullptr;
    wl_list link{};
};
struct AddonSet {
    wl_list addons{};
};
void addon_set_init(AddonSet* set);
// Destroys what's left in it.
void addon_set_finish(AddonSet* set);
void addon_init(Addon* addon, AddonSet* set, const void* owner, const AddonInterface* impl);
void addon_finish(Addon* addon);
Addon* addon_find(AddonSet* set, const void* owner, const AddonInterface* impl);

struct Buffer;
struct BufferImpl {
    void (*destroy)(Buffer* buffer);
    bool (*get_dmabuf)(Buffer* buffer, DmabufAttributes* attribs);
    bool (*get_shm)(Buffer* buffer, ShmAttributes* attribs);
    bool (*begin_data_ptr_access)(Buffer* buffer, uint32_t flags, void** data, uint32_t* format, size_t* stride);
    void (*end_data_ptr_access)(Buffer* buffer);
};

struct Buffer {
    const BufferImpl* impl = nullptr;
    int width = 0, height = 0;
    bool dropped = false;
    size_t n_locks = 0;
    bool accessing_data_ptr = false;
    struct {
        wl_signal destroy;  // it is going
        wl_signal release;  // its last lock went
    } events;
    AddonSet addons;
};

void buffer_init(Buffer* buffer, const BufferImpl* impl, int width, int height);
// For the implementation's destroy: says it's going, frees the addons.
void buffer_finish(Buffer* buffer);
// Its maker lets go: it goes once unlocked.
void buffer_drop(Buffer* buffer);
Buffer* buffer_lock(Buffer* buffer);
void buffer_unlock(Buffer* buffer);
bool buffer_get_dmabuf(Buffer* buffer, DmabufAttributes* attribs);
bool buffer_get_shm(Buffer* buffer, ShmAttributes* attribs);
bool buffer_begin_data_ptr_access(Buffer* buffer, uint32_t flags, void** data, uint32_t* format, size_t* stride);
void buffer_end_data_ptr_access(Buffer* buffer);
// Its DRM format, from its dmabuf or shm (DRM_FORMAT_INVALID if neither).
uint32_t buffer_drm_format(Buffer* buffer);
// No alpha to blend: an opaque format, or an opaque single pixel.
bool buffer_is_opaque(Buffer* buffer);

} // namespace atrium
