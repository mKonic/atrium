#include "snapshot.hpp"

#include <gtest/gtest.h>

using namespace atrium;

// A space destroyed under a window's open or minimize animation (a screen
// plugged in rebuilds them) took the snapshot's tree with it, and the next
// frame placed freed nodes (smoke: "a second screen is plugged in", SIGSEGV
// in Snapshot::place).
TEST(Snapshot, OutlivesItsParentTree) {
    wlr_scene* scene = wlr_scene_create();
    wlr_scene_tree* space = wlr_scene_tree_create(&scene->tree);
    wlr_scene_rect* backing = wlr_scene_rect_create(space, 100, 80, std::array<float, 4>{1, 1, 1, 1}.data());
    Snapshot snap(space, {10, 20, 100, 80});
    snap.add_rect(backing, Color{1, 1, 1, 1});
    snap.place({0, 0, 50, 40}, 0.5f);

    wlr_scene_node_destroy(&space->node);
    EXPECT_EQ(snap.tree(), nullptr);
    snap.place({0, 0, 100, 80}, 1);
    snap.motion(4, 0, 3);
    snap.warp([](double x, double y) { return FPoint{x, y}; }, 1);
    // And the destructor doesn't destroy it a second time.
    wlr_scene_node_destroy(&scene->tree.node);
}
