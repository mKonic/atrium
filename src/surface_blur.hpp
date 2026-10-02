#pragma once
#include "scene/scene.hpp"

namespace atrium::wl {
class Surface;
}

namespace atrium {

class Server;

// The buffer node showing `surface` itself (not its subsurfaces) in `tree`.
scene::Buffer* main_buffer(scene::Tree* tree, wl::Surface* surface);

// Frosted glass behind a popup (a menu, an input method's candidates) while
// transparency is on: only where the popup draws (its own buffer is the
// mask), over whatever is under it, its window included. Kept up to date on
// every commit; gone with `tree`.
void attach_surface_blur(Server& server, scene::Tree* tree, wl::Surface* surface);
// Every popup's blur again, after the settings changed.
void refresh_surface_blurs();

} // namespace atrium
