#pragma once
// What a screen showed when atrium took it over: the boot splash Plymouth
// left up, or the greeter's last frame (wlroots leaves it with CLOSEFB).
// Shown as atrium's first frame in the same mode, so taking the screen over
// is no modeset and no black, and then cross-faded into atrium's own.
#include "wlr.hpp"

namespace atrium {

struct Scanout {
    wlr_buffer* buffer = nullptr;   // the framebuffer's planes as dmabufs; the caller drops it
    wlr_output_mode* mode = nullptr;  // the output's mode the screen is in
};

// Nothing (no buffer) for a screen that's off, one that isn't DRM's, or a
// driver that keeps its framebuffers to itself.
Scanout capture_scanout(wlr_output* output);

} // namespace atrium
