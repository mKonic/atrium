# atrium

A floating-window Wayland compositor for Linux, built on wlroots and scenefx.

## Features

- Floating windows, centered on open, cascaded when stacked
- Click to focus and raise
- Rounded corners and soft shadows on every window
- Move and resize by the app's own title bar and edges, or Mod + drag
- Edge snapping while dragging
- Maximize, fullscreen and minimize
- Panels, docks and wallpapers (wlr-layer-shell)
- Screen locking (ext-session-lock)
- Window lists for docks and task switchers (foreign-toplevel)
- Screen and per-window capture
- X11 apps through Xwayland

## Build from source

Dependencies: `wlroots0.20`, `scenefx0.5` (CachyOS repo or AUR), `wayland`, `wayland-protocols`,
`libinput`, `libxkbcommon`, `pixman`, and `xorg-xwayland`, `libxcb` and `xcb-util-wm` for X11
apps. Needs meson and a C++23 compiler. `ghostty` is the terminal `Mod` + `Return` opens.

```sh
meson setup build
ninja -C build
```

## Usage

From a TTY:

```sh
./build/src/atrium
```

Inside a running Wayland or X11 session, atrium opens as a window. `-s CMD` runs a command once
it is up, `-d` turns on verbose logging:

```sh
./build/src/atrium -s foot
```

### Keys

`Mod` is Super, or Alt when atrium runs inside another session.

| Keys | Action |
|---|---|
| `Mod` + `Return` | Open a terminal (ghostty) |
| `Mod` + `Q` | Close window |
| `Mod` + `F` | Toggle fullscreen |
| `Mod` + `Up` | Toggle maximize |
| `Mod` + `H` | Minimize |
| `Mod` + `Tab` / `Mod` + `Shift` + `Tab` | Cycle windows on this output |
| `Mod` + left drag | Move window |
| `Mod` + right drag | Resize from the nearest corner |
| `Mod` + `Shift` + `E` | Quit |
| `Ctrl` + `Alt` + `F1`…`F12` | Switch VT (TTY only) |

Minimized windows come back through a dock or task switcher.

## License

GPL-3.0-or-later. Derived from [dwl](https://codeberg.org/dwl/dwl); see `LICENSE`.
