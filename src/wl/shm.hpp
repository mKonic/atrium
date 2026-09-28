#pragma once
#include "wl/buffer.hpp"

#include <memory>
#include <vector>

namespace atrium::wl {

// wl_shm: buffers in memory the client shares through a file. Each pool is
// mapped once; buffers read through the pool's mapping, which follows it when
// the pool grows. A client that shrinks the file under us gets a protocol
// error instead of crashing the compositor (the SIGBUS is caught).
class Shm {
public:
    // `formats`: DRM fourccs the renderer can take from memory. ARGB8888 and
    // XRGB8888 are always offered, as the protocol requires.
    Shm(wl_display* display, std::vector<uint32_t> formats);
    ~Shm();
    Shm(const Shm&) = delete;
    Shm& operator=(const Shm&) = delete;

    bool supports(uint32_t drm_format) const;

    // wl_shm's format codes are DRM's, except for the two it had first.
    static uint32_t to_drm(uint32_t wl_format);
    static uint32_t from_drm(uint32_t drm_format);

private:
    std::shared_ptr<const std::vector<uint32_t>> formats_;
    std::unique_ptr<Global> global_;
    std::vector<Weak<WlShm>> shms_;
};

} // namespace atrium::wl
