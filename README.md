<div align = center>

<br>

atrium is a floating Wayland compositor that keeps windows where apps expect them.

It gives every window rounded corners and soft shadows, and keeps your own keybinds on top.

<br>

</div>

# Features

- Floating windows that open centered and cascade
- Rounded corners and soft shadows
- Click to focus, drag to move, Mod + drag to move or resize
- Edge snapping
- Maximize, fullscreen and minimize
- Panels, docks and wallpapers
- Screen locking
- Window lists for docks and task switchers
- Screen and per-window capture
- X11 apps through Xwayland

# Building

```sh
meson setup build
ninja -C build
./build/src/atrium
```

# Special Thanks

**[dwl]** - *For the base atrium grew from*

**[wlroots]** - *For powering atrium*

**[scenefx]** - *For the eyecandy*

**[labwc]** - *For showing how floating windows should behave*

<!----------------------------------------------------------------------------->

[dwl]: https://codeberg.org/dwl/dwl
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
[scenefx]: https://github.com/wlrfx/scenefx
[labwc]: https://github.com/labwc/labwc
