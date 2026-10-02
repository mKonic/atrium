#pragma once
// The C headers and atrium basics most of the compositor uses, in one place.

#include <linux/input-event-codes.h>

#include "util/box.hpp"
#include "util/buffer.hpp"
#include "util/log.hpp"
#include "util/region.hpp"

#include <libinput.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <sys/types.h>

#ifdef ATRIUM_XWAYLAND
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#endif

// The protocols' enums (ZWLR_LAYER_SHELL_V1_LAYER_TOP and the like).
#include "xdg-shell-protocol.h"
// It names a parameter `namespace`.
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-protocol.h"
#undef namespace

#include "render/fwd.hpp"
