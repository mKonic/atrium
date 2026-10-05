#include "scene/dump.hpp"
#include "scene/scene.hpp"
#include "util/buffer.hpp"

#include <gtest/gtest.h>

using namespace atrium::scene;

TEST(SceneDump, TreeAndLeaves) {
    Scene* scene = Scene::create();
    Tree* window = Tree::create(scene);
    window->set_position(100, 50);
    window->set_opacity(0.5f);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1};
    Rect* backing = Rect::create(window, 300, 200, grey);
    backing->set_position(0, 30);
    Blur* blur = Blur::create(window, 300, 230);
    blur->set_use_cache(true);
    blur->set_enabled(false);

    const auto j = dump(window);
    EXPECT_EQ(j["type"], "tree");
    EXPECT_EQ(j["x"], 100);
    EXPECT_EQ(j["y"], 50);
    EXPECT_EQ(j["opacity"], 0.5);
    EXPECT_EQ(j["warped"], false);
    ASSERT_EQ(j["children"].size(), 2u);
    const auto& r = j["children"][0];  // bottom to top
    EXPECT_EQ(r["type"], "rect");
    EXPECT_EQ(r["x"], 100);
    EXPECT_EQ(r["y"], 80);  // placed in layout pixels
    EXPECT_EQ(r["width"], 300);
    EXPECT_EQ(r["color"][3], 1);
    const auto& b = j["children"][1];
    EXPECT_EQ(b["type"], "blur");
    EXPECT_EQ(b["source"], "desktop");
    EXPECT_EQ(b["enabled"], false);
    EXPECT_EQ(b["shown"], false);

    window->set_warp([](double u, double v) { return std::pair{u, v}; }, {100, 50, 300, 230});
    EXPECT_EQ(dump(window)["warped"], true);
    scene->destroy();
}

TEST(SceneDump, BuffersSayHowTheyreCut) {
    static const atrium::BufferImpl impl = {
        .destroy = [](atrium::Buffer* b) { atrium::buffer_finish(b); delete b; },
    };
    auto* pixels = new atrium::Buffer{};
    atrium::buffer_init(pixels, &impl, 640, 360);
    Scene* scene = Scene::create();
    Buffer* b = Buffer::create(scene, pixels);
    atrium::buffer_drop(pixels);  // the node holds it now
    const atrium::FBox crop{10, 20, 320, 180};
    b->set_source_box(&crop);
    b->set_dest_size(160, 90);

    const auto j = dump(b);
    EXPECT_EQ(j["type"], "buffer");
    EXPECT_EQ(j["buffer"]["width"], 640);
    EXPECT_EQ(j["buffer"]["height"], 360);
    EXPECT_EQ(j["src"], (nlohmann::json{10, 20, 320, 180}));
    EXPECT_EQ(j["dst"], (nlohmann::json{160, 90}));
    EXPECT_EQ(j["width"], 160);
    EXPECT_FALSE(j.contains("single_pixel"));
    EXPECT_EQ(j["surface"], false);
    scene->destroy();
}
