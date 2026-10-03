#pragma once
// What a part of the scene is, as JSON (IPC scene.dump): for tests that
// check what's drawn (blur under a window, its shadow, a warp) without
// comparing pixels.

#include <nlohmann/json.hpp>

namespace atrium::scene {

class Node;

// `node` and everything under it: each node's type, where it shows (layout
// pixels, its size scaled), whether it's on, and what its kind adds (a
// tree's opacity, scale and whether it's warped or moving; a blur's
// strength and source; a rect's colour).
nlohmann::json dump(const Node* node);

} // namespace atrium::scene
