#pragma once
// What's drawn, as JSON, for tests (scene.dump, `atriumctl scene [ID]`):
// each node's type, whether it and every parent are enabled ("shown"), its
// box in layout coordinates, and what it is (a blur's source, a rect's
// colour), trees with their children. The checks assert on this instead of
// on pixels.
#include "wlr.hpp"

#include <nlohmann/json.hpp>

namespace atrium {

nlohmann::json dump_scene(wlr_scene_node* node);

} // namespace atrium
