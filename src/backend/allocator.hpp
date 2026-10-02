#pragma once
// Buffers to render frames into: GBM buffer objects on the render device,
// handed around as wlr_buffers (the renderer and the backends take those).
#include "render/fwd.hpp"
#include "util/buffer.hpp"
#include <cstdint>
#include <memory>
#include <vector>

extern "C" {
#include <wayland-server-core.h>
}

struct gbm_device;

namespace atrium::backend {

class Allocator {
public:
    // On `drm_fd` (duplicated; the caller keeps its own). Null without GBM.
    static std::unique_ptr<Allocator> create(int drm_fd);
    // Dumb buffers on `drm_fd`: linear, CPU-written, for scan-out only.
    static std::unique_ptr<Allocator> create_dumb(int drm_fd);
    ~Allocator();
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    // A buffer of `format` with one of `modifiers` (empty: implicit), not
    // locked: the caller locks it, and drops it when done.
    Buffer* allocate(int width, int height, uint32_t format, const std::vector<uint64_t>& modifiers);
    int fd() const { return fd_; }

private:
    Allocator() = default;
    Buffer* allocate_dumb(int width, int height, uint32_t format);
    int fd_ = -1;
    gbm_device* gbm_ = nullptr;
};

// The buffers a screen cycles through: one being shown, one queued, one drawn.
class Swapchain {
public:
    Swapchain(Allocator& allocator, int width, int height, uint32_t format, std::vector<uint64_t> modifiers);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // A buffer nobody holds, locked for the caller (null if none can be made).
    Buffer* acquire();
    bool has(const Buffer* buffer) const;

    const int width, height;
    const uint32_t format;
    const std::vector<uint64_t> modifiers;

private:
    static constexpr size_t kSlots = 4;
    struct Slot {
        Buffer* buffer = nullptr;
        bool acquired = false;
        wl_listener release{};
        Swapchain* owner = nullptr;
    };
    Allocator& allocator_;
    Slot slots_[kSlots];
};

} // namespace atrium::backend
