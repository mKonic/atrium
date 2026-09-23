#pragma once
#include "wlr.hpp"

#include "rules.hpp"

#include <array>
#include <string>
#include <vector>

namespace atrium {

using Color = std::array<float, 4>;

// Scene rects take premultiplied color: straight (1, 1, 1, 0.1) would draw
// as solid white. Settings store straight alpha; convert at the scene.
inline Color premultiplied(Color c) {
    return {c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]};
}

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
    Space,            // iarg: space number on the focused output
    MoveToSpace,      // iarg: space number
    SpacePrev,
    SpaceNext,
    ToggleSecret,     // arg: secret space name
    MoveToSecret,     // arg: secret space name
    SnapLeft,
    SnapRight,
    Restore,          // out of fullscreen, maximized or snapped
    Overview,         // every window of the space, side by side
    SwitchNext,       // Alt+Tab: hold the modifier, tap to walk the windows
    SwitchPrev,
    CycleSpaceNext,   // existing spaces on the output, wrapping around
    CycleSpacePrev,
};

struct Keybind {
    uint32_t mods;
    xkb_keysym_t sym;
    Action action;
    std::string arg{};
    int iarg = 0;
};

// Caps Lock and Num Lock never change which binding a key means.
constexpr uint32_t clean_mods(uint32_t mods) {
    return mods & ~uint32_t(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
}

// The binding for `sym` pressed with `mods`, or null.
const Keybind* find_keybind(const std::vector<Keybind>& binds, uint32_t mods, xkb_keysym_t sym);

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
    float shadow_sigma = 28.0f;
    float shadow_sigma_inactive = 14.0f;
    Color shadow_color{0.0f, 0.0f, 0.0f, 0.55f};
    Color shadow_color_inactive{0.0f, 0.0f, 0.0f, 0.25f};
    Color secret_backdrop{0.0f, 0.0f, 0.0f, 0.45f};
    Color outline_color{1.0f, 1.0f, 1.0f, 0.11f};
    Color outline_color_inactive{1.0f, 1.0f, 1.0f, 0.06f};

    // Blur
    bool transparency = false;  // translucent windows show through; off backs them with a solid fill
    bool blur = true;
    int blur_radius = 6;
    int blur_passes = 3;

    // Motion
    bool animations = true;
    double animation_speed = 1.0;  // multiplier: 2 = twice as fast

    // Windows
    int snap_distance = 16;   // px from a screen edge where a dragged window sticks
    int cascade_step = 28;    // offset for a new window that would cover another exactly
    bool snapping = true;     // drag to screen edges and corners to tile
    int snap_gap = 8;         // between and around snapped windows
    bool tiled_titlebars = true;  // snapped/tiled windows keep their title bar
    bool remember_placement = true;  // an app reopens where its window last closed

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

    // Window rules, applied when a window opens
    std::vector<WindowRule> rules;

    // Bindings
    uint32_t mod = WLR_MODIFIER_LOGO;
    std::string terminal = "ghostty";
    std::vector<Keybind> keybinds;

    // `nested` = running as a window inside another compositor, which owns
    // Super; Alt is used as the modifier there instead.
    static Config defaults(bool nested);
};

} // namespace atrium
