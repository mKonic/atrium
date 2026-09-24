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
    AppExpose,        // the overview with one app's windows; arg: app id, or
                      // "window:ID" for that window's app; none: the focused one's
    SwitchNext,       // Alt+Tab: hold the modifier, tap to walk the windows
    SwitchPrev,
    CycleSpaceNext,   // existing spaces on the output, wrapping around
    CycleSpacePrev,
    RestartShell,     // the bar, dock and the rest of the desktop shell
    Shell,            // arg: something the shell shows ("launcher")
    ToggleTiling,     // the current space tiles its windows, or floats them again
    FocusDirection,   // arg: left, right, up, down: the nearest window that way
    MoveDirection,    // arg: the same; tiled: trade places; floating: snap that way
    MoveToSpacePrev,  // the window to the space before or after, and follow it
    MoveToSpaceNext,
    ToggleFloating,   // on a tiled space: out of the tiles and back
    TogglePin,        // on every space (sticky)
    NextLayout,       // the next keyboard layout
    Portal,           // arg: "APP/ID", an app's global shortcut (the portal): held and let go
};

struct Keybind {
    uint32_t mods;
    xkb_keysym_t sym;
    Action action;
    std::string arg{};
    int iarg = 0;
    bool locked = false;  // also works on the lock screen (media keys)
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
    int secret_margin = 5;  // percent of the screen left around secret-space windows
    Color outline_color{1.0f, 1.0f, 1.0f, 0.11f};
    Color outline_color_inactive{1.0f, 1.0f, 1.0f, 0.06f};

    // Blur
    bool light = false;  // appearance.style: light chrome, shell and apps
    std::string accent = "multicolor";  // appearance.accent: accent::names()
    bool liquid_glass = false;           // appearance.liquid_glass: glass panels
    double glass_refraction = 14;        // how far it bends light at the edge, px
    double glass_frost = 0.3;            // its blur, as a share of the frosted one
    bool transparency = false;  // translucent windows show through; off backs them with a solid fill
    bool blur = true;
    std::vector<std::string> blurred_panels{"atrium-*"};  // layer namespaces frosted behind (with transparency)
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
    bool fullscreen_space = false;  // a window going fullscreen gets a space of its own
    bool remember_placement = true;  // an app reopens where its window last closed

    // Keyboard
    // Empty layout: the system's keyboard (keyboard_conf.hpp).
    std::string xkb_rules, xkb_model, xkb_layout, xkb_variant, xkb_options;
    // xkb options of their own, added to xkb_options: the keys that switch
    // layouts ("grp:alt_shift_toggle") and the compose key ("compose:ralt").
    std::string xkb_switch, xkb_compose;
    bool layout_per_window = false;  // each window keeps the layout it was typed in
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
    std::string icon_theme, font, mono_font;  // empty: what apps already use
    int font_size = 11;
    int cursor_size = 24;

    // Idle inhibitors from hidden windows still count (a video in a
    // background window keeps the screen on).
    bool idle_inhibit_ignore_visibility = false;

    // Window rules, applied when a window opens
    std::vector<WindowRule> rules;

    // Bindings
    uint32_t mod = WLR_MODIFIER_LOGO;
    std::string terminal;  // empty: kDefaultTerminal
    std::string shell = "builtin";  // desktop shell: "builtin", a command, or "none"
    bool clipboard_history = true;  // record copies with cliphist for the Super+V picker
    std::string power_profile = "performance";  // power-profiles-daemon profile set at login
    bool allow_tearing = false;  // fullscreen games that ask may skip vblank
    int brightness = 100;           // external monitors (DDC/CI), restored at login
    std::string night_light = "off";  // off, sunset (to sunrise), custom (from/to), always
    int night_light_warmth = 50;       // 0 a little warmer, 100 a lot
    std::string night_light_from = "22:00", night_light_to = "07:00";
    std::vector<Keybind> keybinds;

    // `nested` = running as a window inside another compositor, which owns
    // Super; Alt is used as the modifier there instead.
    static Config defaults(bool nested);

    // `atrium --greeter`: the login screen under greetd. The shell is the
    // greeter, nothing is remembered, and no shortcut starts anything.
    bool greeter = false;
};

} // namespace atrium
