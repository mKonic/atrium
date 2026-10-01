// wp_color_manager_v1 (src/wl/color) against a real libwayland client.
#include "wl/color.hpp"
#include "wl/compositor.hpp"
#include "wl/output.hpp"
#include "wl_harness.hpp"

#include "color-management-v1-client-protocol.h"

#include <wayland-client-protocol.h>

#include <gtest/gtest.h>

using namespace atrium;

namespace {

using CM = wl::ColorManagement;

CM::Options options() {
    return {.intents = {WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL},
            .features = {WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC,
                         WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES},
            .transfer_functions = {WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
                                   WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ},
            .primaries = {WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020}};
}

struct Color : wltest::Harness {
    wl::Compositor compositor{server, nullptr};
    wl::Output output{server, wl::OutputInfo{.name = "DP-1"}};
    CM color{server, options()};
    std::vector<wl::Surface*> surfaces;
    wl::Connection made = compositor.new_surface.connect([this](wl::Surface* s) { surfaces.push_back(s); });
    wl_compositor* comp = nullptr;
    wp_color_manager_v1* cm = nullptr;
    std::vector<uint32_t> tfs;
    int done = 0;
    Color() {
        comp = bind<wl_compositor>(&wl_compositor_interface);
        cm = bind<wp_color_manager_v1>(&wp_color_manager_v1_interface, 1);
        static const wp_color_manager_v1_listener ml = {
            .supported_intent = [](void*, wp_color_manager_v1*, uint32_t) {},
            .supported_feature = [](void*, wp_color_manager_v1*, uint32_t) {},
            .supported_tf_named = [](void* d, wp_color_manager_v1*,
                                     uint32_t tf) { static_cast<Color*>(d)->tfs.push_back(tf); },
            .supported_primaries_named = [](void*, wp_color_manager_v1*, uint32_t) {},
            .done = [](void* d, wp_color_manager_v1*) { ++static_cast<Color*>(d)->done; },
        };
        wp_color_manager_v1_add_listener(cm, &ml, this);
        pump();
    }
    ~Color() {
        if (!client)
            return;
        wp_color_manager_v1_destroy(cm);
        wl_compositor_destroy(comp);
        pump();
    }
};

struct DescLog {
    uint32_t identity = 0;
    int failed = 0;
    uint32_t tf = 0;
    int info_done = 0;
};
const wp_image_description_v1_listener kDesc = {
    .failed = [](void* d, wp_image_description_v1*, uint32_t, const char*) { ++static_cast<DescLog*>(d)->failed; },
    .ready = [](void* d, wp_image_description_v1*, uint32_t id) { static_cast<DescLog*>(d)->identity = id; },
};

} // namespace

TEST(WlColor, AnnouncesWhatItTakes) {
    Color c;
    EXPECT_EQ(c.done, 1);
    EXPECT_EQ(c.tfs.size(), 2u);
}

TEST(WlColor, ParametricDescriptionsApplyWithTheCommit) {
    Color c;
    wl_surface* s = wl_compositor_create_surface(c.comp);
    c.pump();
    wl::Surface* ss = c.surfaces.back();
    auto* creator = wp_color_manager_v1_create_parametric_creator(c.cm);
    wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_mastering_luminance(creator, 50, 1000);
    wp_image_description_v1* desc = wp_image_description_creator_params_v1_create(creator);
    DescLog log;
    wp_image_description_v1_add_listener(desc, &kDesc, &log);
    c.pump();
    EXPECT_NE(log.identity, 0u);

    auto* cs = wp_color_manager_v1_get_surface(c.cm, s);
    wp_color_management_surface_v1_set_image_description(cs, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    c.pump();
    EXPECT_EQ(ss->current().image_description, nullptr);  // not until committed
    wl_surface_commit(s);
    c.pump();
    ASSERT_NE(ss->current().image_description, nullptr);
    EXPECT_EQ(ss->current().image_description->tf_named, uint32_t(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ));
    EXPECT_EQ((*ss->current().image_description->mastering_luminance)[1], 1000u);

    // An equal description gets the same identity.
    EXPECT_EQ(c.color.identity_of(*ss->current().image_description), log.identity);

    wp_color_management_surface_v1_destroy(cs);
    wl_surface_commit(s);
    c.pump();
    EXPECT_EQ(ss->current().image_description, nullptr);  // gone with its object
    wp_image_description_v1_destroy(desc);
    wl_surface_destroy(s);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}

TEST(WlColor, IncompleteOrUnsupportedIsAnError) {
    {
        Color c;
        auto* creator = wp_color_manager_v1_create_parametric_creator(c.cm);
        wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
        wp_image_description_v1* desc = wp_image_description_creator_params_v1_create(creator);
        c.pump();
        EXPECT_TRUE(c.posted("wp_image_description_creator_params_v1", WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET));
        wp_image_description_v1_destroy(desc);  // create consumed the creator
    }
    {
        Color c;
        auto* creator = wp_color_manager_v1_create_parametric_creator(c.cm);
        wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886);
        c.pump();
        EXPECT_TRUE(c.posted("wp_image_description_creator_params_v1", WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF));
        wp_image_description_creator_params_v1_destroy(creator);
    }
}

TEST(WlColor, OutputsTellWhatTheyAre) {
    Color c;
    auto* wo = c.bind<wl_output>(&wl_output_interface, 4);
    auto* co = wp_color_manager_v1_get_output(c.cm, wo);
    int changed = 0;
    static const wp_color_management_output_v1_listener ol = {
        .image_description_changed = [](void* d, wp_color_management_output_v1*) { ++*static_cast<int*>(d); },
    };
    wp_color_management_output_v1_add_listener(co, &ol, &changed);
    c.pump();
    wl::ImageDescription hdr;
    hdr.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
    hdr.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
    c.color.set_output_description(&c.output, hdr);
    c.pump();
    EXPECT_EQ(changed, 1);

    wp_image_description_v1* desc = wp_color_management_output_v1_get_image_description(co);
    DescLog log;
    wp_image_description_v1_add_listener(desc, &kDesc, &log);
    wp_image_description_info_v1* info = wp_image_description_v1_get_information(desc);
    static const wp_image_description_info_v1_listener il = {
        .done =
            [](void* d, wp_image_description_info_v1* info) {
                ++static_cast<DescLog*>(d)->info_done;
                wp_image_description_info_v1_destroy(info);  // done is a destructor event
            },
        .icc_file = [](void*, wp_image_description_info_v1*, int32_t, uint32_t) {},
        .primaries = [](void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                        int32_t, int32_t) {},
        .primaries_named = [](void*, wp_image_description_info_v1*, uint32_t) {},
        .tf_power = [](void*, wp_image_description_info_v1*, uint32_t) {},
        .tf_named = [](void* d, wp_image_description_info_v1*, uint32_t tf) { static_cast<DescLog*>(d)->tf = tf; },
        .luminances = [](void*, wp_image_description_info_v1*, uint32_t, uint32_t, uint32_t) {},
        .target_primaries = [](void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t,
                               int32_t, int32_t, int32_t) {},
        .target_luminance = [](void*, wp_image_description_info_v1*, uint32_t, uint32_t) {},
        .target_max_cll = [](void*, wp_image_description_info_v1*, uint32_t) {},
        .target_max_fall = [](void*, wp_image_description_info_v1*, uint32_t) {},
    };
    wp_image_description_info_v1_add_listener(info, &il, &log);
    c.pump();
    EXPECT_EQ(log.tf, uint32_t(WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ));
    EXPECT_EQ(log.info_done, 1);
    wp_image_description_v1_destroy(desc);
    wp_color_management_output_v1_destroy(co);
    wl_output_release(wo);
    c.pump();
    EXPECT_EQ(c.error(), 0);
}
