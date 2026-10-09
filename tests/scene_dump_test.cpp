#include "scene_dump.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace atrium;
using nlohmann::json;

TEST(SceneDump, NodesWithWhatTheyAreAndWhetherTheyShow) {
    wlr_scene* scene = wlr_scene_create();
    wlr_scene_tree* window = wlr_scene_tree_create(&scene->tree);
    wlr_scene_node_set_position(&window->node, 100, 50);
    wlr_scene_shadow_create(window, 220, 120, 12, 20, std::array<float, 4>{0, 0, 0, 0.5f}.data());
    wlr_scene_blur* desktop = wlr_scene_blur_create(window, 200, 100);
    wlr_scene_blur_set_should_only_blur_bottom_layer(desktop, true);
    wlr_scene_tree* hidden = wlr_scene_tree_create(window);
    wlr_scene_node_set_enabled(&hidden->node, false);
    wlr_scene_blur* below = wlr_scene_blur_create(hidden, 10, 10);
    wlr_scene_blur_set_should_only_blur_bottom_layer(below, false);
    wlr_scene_rect* rect = wlr_scene_rect_create(window, 30, 20, std::array<float, 4>{1, 0.5f, 0, 1}.data());
    wlr_scene_node_set_position(&rect->node, 5, 6);
    wlr_scene_buffer_create(window, nullptr);

    const json d = dump_scene(&window->node);
    EXPECT_EQ(d["type"], "tree");
    ASSERT_EQ(d["children"].size(), 5u);
    EXPECT_EQ(d["children"][0]["type"], "shadow");
    EXPECT_EQ(d["children"][0]["width"], 220);
    const json& b = d["children"][1];
    EXPECT_EQ(b["type"], "blur");
    EXPECT_EQ(b["source"], "desktop");
    EXPECT_TRUE(b["shown"]);
    // Enabled itself, under a disabled parent: not shown.
    const json& nested = d["children"][2]["children"][0];
    EXPECT_EQ(nested["source"], "below");
    EXPECT_TRUE(nested["enabled"]);
    EXPECT_FALSE(nested["shown"]);
    // Layout coordinates, the parent's position included.
    const json& r = d["children"][3];
    EXPECT_EQ(r["x"], 105);
    EXPECT_EQ(r["y"], 56);
    EXPECT_EQ(r["color"][1], 0.5);
    EXPECT_EQ(d["children"][4]["type"], "buffer");
    EXPECT_FALSE(d["children"][4]["has_buffer"]);

    wlr_scene_node_destroy(&scene->tree.node);
}
