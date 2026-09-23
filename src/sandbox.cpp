#include "sandbox.hpp"

#include <array>
#include <string_view>

namespace atrium {

namespace {

constexpr std::array<std::string_view, 20> kPrivileged{
    // Reading the screen and other windows
    "zwlr_screencopy_manager_v1",
    "zwlr_export_dmabuf_manager_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "ext_foreign_toplevel_list_v1",
    "zwlr_foreign_toplevel_manager_v1",
    // The clipboard behind the app's back
    "zwlr_data_control_manager_v1",
    "ext_data_control_manager_v1",
    // Fake input
    "zwp_virtual_keyboard_manager_v1",
    "zwlr_virtual_pointer_manager_v1",
    // Being part of the desktop, or changing it
    "zwlr_layer_shell_v1",
    "ext_session_lock_manager_v1",
    "ext_workspace_manager_v1",
    "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1",
    "zwlr_gamma_control_manager_v1",
    "zwp_keyboard_shortcuts_inhibit_manager_v1",
    "ext_idle_notifier_v1",
    // No nesting a sandbox inside one
    "wp_security_context_manager_v1",
};

} // namespace

bool privileged_protocol(std::string_view interface) {
    for (std::string_view p : kPrivileged)
        if (p == interface)
            return true;
    return false;
}

} // namespace atrium
