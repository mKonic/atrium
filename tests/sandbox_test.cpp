#include "sandbox.hpp"

#include <gtest/gtest.h>

using namespace atrium;

TEST(Sandbox, HidesWhatReadsOrDrivesTheDesktop) {
    for (const char* p : {"zwlr_screencopy_manager_v1", "ext_image_copy_capture_manager_v1", "zwlr_data_control_manager_v1",
                          "zwp_virtual_keyboard_manager_v1", "zwlr_layer_shell_v1", "zwlr_output_manager_v1",
                          "wp_security_context_manager_v1"})
        EXPECT_TRUE(privileged_protocol(p)) << p;
}

TEST(Sandbox, KeepsWhatAnOrdinaryAppNeeds) {
    for (const char* p : {"wl_compositor", "wl_seat", "xdg_wm_base", "wl_data_device_manager", "zxdg_decoration_manager_v1",
                          "wp_fractional_scale_manager_v1", "xdg_activation_v1", "wp_tearing_control_manager_v1",
                          "xdg_toplevel_icon_manager_v1", "zwp_linux_dmabuf_v1", "wp_cursor_shape_manager_v1"})
        EXPECT_FALSE(privileged_protocol(p)) << p;
}
