#pragma once
#include "wlr.hpp"

#include <array>
#include <string>
#include <vector>

namespace atrium {

using Color = std::array<float, 4>;

enum class Action {
    Spawn,            // arg: shell command
    SpawnTerminal,
    CloseWindow,
    ToggleFullscreen,
    ToggleMaximize,
    Minimize,
    FocusNext,
    FocusPrev,
    SwitchVt,         // iarg: VT number
    Quit,
};

struct Keybind {
    uint32_t mods;
    xkb_keysym_t sym;
    Action action;
    std::string arg{};
    int iarg = 0;
};

// Everything the compositor reads from settings, in one place. This is the
// in-memory form of the settings store: today it only holds defaults, later
// the store fills it and pushes live changes into it.
struct Config {
    // Appearance
    Color background{0.10f, 0.11f, 0.13f, 1.0f};
    Color fullscreen_background{0.0f, 0.0f, 0.0f, 1.0f};
    Color lock_background{0.1f, 0.1f, 0.1f, 1.0f};
    int corner_radius = 12;
    bool shadows = true;
    float shadow_sigma = 24.0f;
    float shadow_sigma_inactive = 14.0f;
    Color shadow_color{0.0f, 0.0f, 0.0f, 0.45f};
    Color shadow_color_inactive{0.0f, 0.0f, 0.0f, 0.25f};

    // Windows
    int snap_distance = 16;   // px from a screen edge where a dragged window sticks
    int cascade_step = 28;    // offset for a new window that would cover another exactly

    // Keyboard
    std::string xkb_rules, xkb_model, xkb_layout = "us", xkb_variant, xkb_options;
    int repeat_rate = 35;
    int repeat_delay = 250;

    // Pointer
    double accel_speed = -0.5;
    libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
    bool natural_scroll = true;
    bool touchpad_natural_scroll = true;
    bool tap_to_click = true;
    bool tap_and_drag = true;
    bool drag_lock = false;
    bool disable_while_typing = true;
    bool left_handed = false;
    bool middle_button_emulation = false;
    std::string cursor_theme;  // empty = XCURSOR_THEME / default
    int cursor_size = 24;

    // Idle inhibitors from hidden windows still count (a video in a
    // background window keeps the screen on).
    bool idle_inhibit_ignore_visibility = false;

    // Bindings
    uint32_t mod = WLR_MODIFIER_LOGO;
    std::string terminal = "ghostty";
    std::vector<Keybind> keybinds;

    // `nested` = running as a window inside another compositor, which owns
    // Super; Alt is used as the modifier there instead.
    static Config defaults(bool nested);
};

} // namespace atrium
