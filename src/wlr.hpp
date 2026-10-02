#pragma once
// Every C header atrium uses, made safe for C++.
//
// wlroots headers are written for C11 and trip g++ with `float
// color[static 4]` array parameters (not valid C++), fixed with a scoped
// #define around the offending header. Every
// header those pull in transitively is included first, normally, so the macro
// only ever touches the declarations it is meant for (a stray `static inline`
// helper would otherwise lose its `static`).
//
// atrium has its own scene graph (src/scene) and protocol layer (src/wl):
// only wlroots' backends, renderer types, outputs and input devices are used.

#include <linux/input-event-codes.h>

#include "util/box.hpp"
#include "util/buffer.hpp"
#include "util/log.hpp"
#include "util/region.hpp"

extern "C" {
#include <libinput.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

// Pulled in by wlr_output.h and others, so it has to come first.
#define static
#include <wlr/render/color.h>
#undef static

#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_swapchain_manager.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>
#include <wlr/util/region.h>

#ifdef ATRIUM_XWAYLAND
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <wlr/xwayland/server.h>
#endif
}

// The protocols' enums (ZWLR_LAYER_SHELL_V1_LAYER_TOP and the like).
#include "xdg-shell-protocol.h"
// It names a parameter `namespace`.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-protocol.h"
#undef namespace

#include "render/fwd.hpp"

// Where atrium's boxes meet wlroots' structs (until wlroots goes, PLAN 88).
namespace atrium {
inline wlr_box to_wlr(const Box& b) {
    return {b.x, b.y, b.width, b.height};
}
inline wlr_fbox to_wlr(const FBox& b) {
    return {b.x, b.y, b.width, b.height};
}
inline Box from_wlr(const wlr_box& b) {
    return {b.x, b.y, b.width, b.height};
}
inline FBox from_wlr(const wlr_fbox& b) {
    return {b.x, b.y, b.width, b.height};
}
} // namespace atrium
