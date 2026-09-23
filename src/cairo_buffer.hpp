#pragma once
#include "wlr.hpp"

#include <cairo.h>

namespace atrium {

// Put a cairo image surface (ARGB32, flushed) into `node`, shown at
// `width` x `height` logical pixels. The buffer takes ownership of `surface`.
//
// Returns the buffer locked for the caller. The scene lets go of its own
// lock once the texture is uploaded, and node->buffer turns null with it; a
// node whose buffer gets copied elsewhere (close animation, overview) has to
// be kept alive by this lock. Release with wlr_buffer_unlock.
wlr_buffer* set_cairo_buffer(wlr_scene_buffer* node, cairo_surface_t* surface, int width, int height);

} // namespace atrium
