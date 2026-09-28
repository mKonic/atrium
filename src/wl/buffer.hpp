#pragma once
#include "wayland-server.hpp"

#include "wlr.hpp"

namespace atrium::wl {

// A client's wl_buffer, whichever protocol made it (shm, dmabuf, single
// pixel): each derives from this. What the renderer draws is its wlr_buffer
// (until the renderer has a buffer type of its own), which can outlive the
// wl_buffer: it goes once the client destroyed the buffer and nobody holds
// a lock on it. The client hears wl_buffer.release whenever the last lock
// goes.
class ClientBuffer : public WlBuffer {
public:
    // The buffer behind a wl_buffer, or null for one that isn't ours.
    static ClientBuffer* from(wl_resource* resource);

    wlr_buffer* buffer() const { return buffer_; }
    int width() const { return buffer_->width; }
    int height() const { return buffer_->height; }

protected:
    // `buffer` is the kind's own wlr_buffer, initialised: this takes it over,
    // and drops it when the wl_buffer goes (it is freed then, or once the
    // last lock goes).
    ClientBuffer(wl_client* client, uint32_t version, uint32_t id, wlr_buffer* buffer);
    ~ClientBuffer() override;

private:
    struct Tracker;
    wlr_buffer* buffer_;
    Tracker* tracker_;
};

// Takes a lock on `buffer` for as long as it lives.
class BufferRef {
public:
    BufferRef() = default;
    explicit BufferRef(wlr_buffer* buffer) : buffer_(buffer ? wlr_buffer_lock(buffer) : nullptr) {}
    BufferRef(const BufferRef& other) : BufferRef(other.buffer_) {}
    BufferRef& operator=(const BufferRef& other) {
        if (this != &other)
            reset(other.buffer_);
        return *this;
    }
    BufferRef(BufferRef&& other) noexcept : buffer_(std::exchange(other.buffer_, nullptr)) {}
    BufferRef& operator=(BufferRef&& other) noexcept {
        if (this != &other) {
            reset();
            buffer_ = std::exchange(other.buffer_, nullptr);
        }
        return *this;
    }
    ~BufferRef() { reset(); }

    void reset(wlr_buffer* buffer = nullptr) {
        if (buffer)
            wlr_buffer_lock(buffer);
        if (buffer_)
            wlr_buffer_unlock(buffer_);
        buffer_ = buffer;
    }
    wlr_buffer* get() const { return buffer_; }
    explicit operator bool() const { return buffer_ != nullptr; }

private:
    wlr_buffer* buffer_ = nullptr;
};

} // namespace atrium::wl
