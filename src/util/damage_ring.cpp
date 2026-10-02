#include "util/damage_ring.hpp"

#include "util/box.hpp"

#include <algorithm>

namespace atrium {

namespace {

constexpr int kMaxRects = 20;  // more than this: their bounding box

struct Entry {
    wl_list link;
    DamageRing* ring;
    Buffer* buffer;
    pixman_region32_t damage;  // between this buffer and the newer one
    wl_listener destroy;
};

void entry_destroy(Entry* e) {
    wl_list_remove(&e->destroy.link);
    wl_list_remove(&e->link);
    pixman_region32_fini(&e->damage);
    delete e;
}

// Its damage goes to the newer neighbour (or the ring's current).
void entry_squash(Entry* e) {
    pixman_region32_t* prev;
    if (e->link.prev == &e->ring->buffers) {
        prev = &e->ring->current;
    } else {
        Entry* newer = wl_container_of(e->link.prev, newer, link);
        prev = &newer->damage;
    }
    pixman_region32_union(prev, prev, &e->damage);
}

} // namespace

void damage_ring_init(DamageRing* ring) {
    pixman_region32_init(&ring->current);
    wl_list_init(&ring->buffers);
}

void damage_ring_finish(DamageRing* ring) {
    pixman_region32_fini(&ring->current);
    Entry *e, *tmp;
    wl_list_for_each_safe(e, tmp, &ring->buffers, link) entry_destroy(e);
}

void damage_ring_add(DamageRing* ring, const pixman_region32_t* damage) {
    pixman_region32_union(&ring->current, &ring->current, const_cast<pixman_region32_t*>(damage));
}

void damage_ring_add_box(DamageRing* ring, const Box* box) {
    pixman_region32_union_rect(&ring->current, &ring->current, box->x, box->y, unsigned(box->width),
                               unsigned(box->height));
}

void damage_ring_add_whole(DamageRing* ring) {
    int w = 0, h = 0;
    Entry* e;
    wl_list_for_each(e, &ring->buffers, link) {
        w = std::max(w, e->buffer->width);
        h = std::max(h, e->buffer->height);
    }
    pixman_region32_union_rect(&ring->current, &ring->current, 0, 0, unsigned(w), unsigned(h));
}

void damage_ring_rotate_buffer(DamageRing* ring, Buffer* buffer, pixman_region32_t* damage) {
    pixman_region32_copy(damage, &ring->current);
    Entry* e;
    wl_list_for_each(e, &ring->buffers, link) {
        if (e->buffer != buffer) {
            pixman_region32_union(damage, damage, &e->damage);
            continue;
        }
        pixman_region32_intersect_rect(damage, damage, 0, 0, unsigned(buffer->width), unsigned(buffer->height));
        if (pixman_region32_n_rects(damage) > kMaxRects) {
            const pixman_box32_t* x = pixman_region32_extents(damage);
            pixman_region32_union_rect(damage, damage, x->x1, x->y1, unsigned(x->x2 - x->x1),
                                       unsigned(x->y2 - x->y1));
        }
        entry_squash(e);
        pixman_region32_copy(&e->damage, &ring->current);
        pixman_region32_clear(&ring->current);
        wl_list_remove(&e->link);
        wl_list_insert(&ring->buffers, &e->link);
        return;
    }
    // A buffer not seen before: all of it.
    pixman_region32_clear(damage);
    pixman_region32_union_rect(damage, damage, 0, 0, unsigned(buffer->width), unsigned(buffer->height));
    e = new Entry();
    pixman_region32_init(&e->damage);
    pixman_region32_copy(&e->damage, &ring->current);
    pixman_region32_clear(&ring->current);
    wl_list_insert(&ring->buffers, &e->link);
    e->buffer = buffer;
    e->ring = ring;
    e->destroy.notify = [](wl_listener* l, void*) {
        Entry* entry = wl_container_of(l, entry, destroy);
        entry_squash(entry);
        entry_destroy(entry);
    };
    wl_signal_add(&buffer->events.destroy, &e->destroy);
}

} // namespace atrium
