#pragma once
// What changed since each buffer of a swapchain was last drawn, so a frame
// redraws only that. After wlroots' types/wlr_damage_ring.c (MIT).
#include "util/box.hpp"
#include "util/buffer.hpp"

#include <pixman.h>

namespace atrium {

struct DamageRing {
    // Since the last frame (buffer-local coordinates).
    pixman_region32_t current;
    wl_list buffers;  // DamageRingBuffer.link, most recent first
};

void damage_ring_init(DamageRing* ring);
void damage_ring_finish(DamageRing* ring);
void damage_ring_add(DamageRing* ring, const pixman_region32_t* damage);
void damage_ring_add_box(DamageRing* ring, const Box* box);
// Everything, as big as the largest buffer seen.
void damage_ring_add_whole(DamageRing* ring);
// About to draw into `buffer`: `damage` is what it lacks (all of it the
// first time), and the ring moves on.
void damage_ring_rotate_buffer(DamageRing* ring, Buffer* buffer, pixman_region32_t* damage);

} // namespace atrium
