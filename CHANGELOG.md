# Changelog

All notable changes to this project will be documented in this file.

The format is a modified version of [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
- `Added` - for new features.
- `Changed` - for changes in existing functionality.
- `Improved` - for enhancement or optimization in existing functionality.
- `Removed` - for now removed features.
- `Fixed` - for any bug fixes.
- `Other` - for technical stuff.

## [Unreleased]

## [v0.1.0] - 2026-09-27

### Changed
- Dragging the brightness slider no longer stalls the desktop: monitors get the new
  brightness straight over DDC/CI (ddcutil held the NVIDIA display driver for a fifth of
  a second each time). With HDR on, the slider sets how bright everything that isn't HDR
  is, as the monitor fixes its own brightness in HDR.
- In HDR, Chrome and other color-managed apps are as bright as everything else (they sat
  at 203 nits), and SDR brightness reaches the screen's peak again.
- The Brightness slider in Settings sets the monitors when let go (it only saved the
  number, used at the next login).
- Clicking the clock in the bar opens the month: today marked, other months by the
  chevrons, the arrow keys or the wheel, Today to come back.
- Turning tiling on takes maximized windows into the tiles too; turning it off puts them
  back where they were before they were maximized.
- Liquid Glass panels no longer show a faint square around their rounded corners (the
  shadow was cut off at the panel's box).
- Dragging a display slider no longer stalls the pointer: brightness changes skip the
  display modeset, and settings are written without a disk sync each time.
- Ghostty (and any GTK app running several windows in one process) no longer crashes,
  taking all its windows, when one closes: atrium's own window-icon handling never hands
  icon pictures back early, which GTK freed twice.
- An app remembered maximized opens as a tile in a tiled space, and a tile's size is no
  longer remembered as the app's window size.
- The space slide also runs to and from an empty space; a fullscreen window's black
  backdrop slides with its space; going fullscreen, the bar and Dock slide away under
  the app instead of flashing over it.
- Fullscreen, bringing the menu bar over also brings the window's title bar out below
  it, usable, as on a Mac.
- Hiding a secret space with the mouse moving no longer brings it back; with a fullscreen
  window in it, hiding it shows the desktop again instead of a black screen.
- The focus ring and every rounded outline keep their edges at every position (the left
  edge of a tile's ring came and went).
- The Dock's name line no longer flickers between icons or over the name.
- Notifications stay answerable after their banner goes: clicking one in the Notification
  Center, or one of its buttons, reaches the app; the center grows with an expanding card.
- A mouse drag no longer scrolls lists and pages (the wheel does), so sliders keep it.
- Closing System Settings (Super+Q, the Dock) quits it; it stayed running hidden, and after
  an update kept showing the old one.
- Tab steps through the fields of Settings' sheets.
- The password prompt for sudo names the app it was typed in and the command, with the
  field ready; the polkit prompt shares the look.
- Keys from on-screen keyboards and typing tools go through an active input method.
- Notification banners come in at the top and push the others down, the oldest sliding
  away, instead of all sliding in again together; the Notification Center's glass is cut
  cleanly where its list scrolls, with nothing past its edges.
- The password prompts (sudo, polkit) float on Liquid Glass over the desktop, with no
  dimmed screen.
- The login password opens the KDE wallet (Chrome's and Spotify's keys) in an atrium
  session, as it does in Plasma.
- In HDR, normal content is never brighter than the screen's peak.

- Tiles and windows in a secret space belong to the layout, as in caelestia: only Mod +
  drag moves them and they glide back into place, and apps' own title-bar drags, resize
  edges, maximize and minimize are ignored there. A secret space's windows fill the screen
  less a margin, maximized or not, instead of the space between the bar and the Dock.
- The menu bar is a wall: a floating window's top can't go above or under it, however it
  got there.
- Every resizable floating window can be resized from its edges, including apps that draw
  their own frame (Discord and other Electron apps have no resize edges of their own).
- The green title-bar button enters full screen (bar, title bar and Dock gone), as on a
  Mac; Alt-click zooms.
- Animations follow caelestia's: the space slide, secret spaces coming down as they fade
  in, windows opening, closing and minimizing, the tiling dim, and windows atrium places
  (tiles, snaps, full screen) gliding there instead of jumping.
- Night light is drawn by the renderer in linear light instead of the screen's gamma
  table, which NVIDIA's driver mangled.
- Dropped frames on DP-1 ("page-flip already pending", over a hundred a session), which
  could leave stale patches on screen, are gone; Liquid Glass redraws whole when something
  under it changes.
- Notifications grow and shrink smoothly when expanded, and Clear All fades them out
  before the Notification Center closes up, instead of flickering and snapping. Clicking
  one brings the app that sent it forward, even from a secret space.
- The Dock, desktop and window menus stay open over apps that retitle themselves
  constantly (Ghostty) and close on a click anywhere else; so do the Control and
  Notification Centers.
- The Dock magnifies under the pointer over the icons, not only below them, and shows the
  hovered app's name inside the shelf instead of floating above it.
- Settings dropdowns open upward when there's no room below, and an app's details scroll
  into view when opened.
- Settings and the welcome are laid out as macOS 26's windows: no title bar, their own
  traffic lights (on the right, as atrium's), and in Settings a sidebar pane inset in the
  window with a fine light rim. Slider and switch knobs are solid capsules that turn to
  clear glass over the track while held.
- Control Center follows macOS 26's: each control its own piece of glass on a grid, with
  no panel behind them: capsules for Wi-Fi and Bluetooth (the icon switches, the rest
  opens the list), round buttons that turn white when on, a square for what's playing,
  and Display and Sound as thin white tracks between two icons.
- A secret space dims what's behind it by 20%, unblurred, as caelestia's special
  workspaces; maximizing a window there keeps it in the space's frame.
- Super + Tab and Super + Shift + Tab step to the next and previous space, empty or not,
  and Super + D starts Discord when it isn't running, as in caelestia.
- Rounded window corners, shadows, borders and window contents are as sharp on the right
  side of a wide screen as on the left (on NVIDIA they were drawn at half resolution past
  1024 pixels).
- With Liquid Glass the menu bar's logo and the app name sit on glass of their own, so they
  read over any wallpaper; the Apple-style menu closes with Escape and grows to fit long
  items; Settings keeps the open page in view in its sidebar and greys a hint into empty
  fields ("Default terminal"). Its cards are white in light mode and a step lighter than
  the window in dark, as on macOS, and slider and switch knobs are raised with a fine edge.
  The welcome names the web browser in use instead of showing its desktop id.
- Colours follow macOS: neutral greys, black or white text and fills at Apple's
  opacities, the system colours for errors and warnings, and Multicolour as the system
  blue. Control Center toggles light up their icon instead of the whole tile. Qt and GTK
  apps get the same colours.
- A long window title in the menu bar gives way before the right side instead of running
  under it.
- The keyboard follows the system's layout (set with `localectl` or by the installer)
  until one is picked in Settings.
- The terminal shortcut, "Open in Terminal" and apps that run in a terminal (btop) use
  your default terminal, else the first one installed, unless Settings names one.
- The shell runs on atrium's own host, atrium-shell, instead of Quickshell, with its own
  notification server, polkit agent, greetd login, media, tray, sound, Bluetooth and network
  services. Quickshell is no longer needed.
- Tray menus open in the shell's style under their icon, with submenus in place.
- Apps that draw their own title bar (Chromium's tab strip) keep it instead of getting
  atrium's on top of theirs.
- Over a fullscreen app the bar and the Dock wait under it instead of over it, so the app
  keeps the top and bottom of the screen for its own pointer, and they come over when the
  pointer is pushed against the top or bottom edge (not while a game holds the pointer).
  A fullscreen game is sent to the screen directly instead of being redrawn by atrium.
- Frames reach the screen a refresh sooner: atrium composites just before the screen's
  refresh instead of right after it, unless variable refresh or tearing already shows a
  frame at once.
- The Dock opens and closes around apps that come and go (an icon grows in, and a closed
  app shrinks away) instead of snapping, however fast they come and go; the bar's space
  highlight and the spaces follow changes smoothly instead of restarting their animation.

### Added
- Super+Space is a command palette, after Tinycast: apps, Settings pages, open windows,
  your quicklinks, snippets and commands, system actions and window commands, found by a
  ranking that learns what you open. With nothing typed: favorites (Ctrl+1 to Ctrl+0),
  suggestions and a section per kind. Ctrl+K on any row for what else it does (keep in
  the Dock, favorite, hide from search, quit or restart the app, uninstall it). A typed
  address opens in the browser; anything else can go to Search Files, a quicklink or the
  shell. Clipboard History (Super+V, or Tab) and Emoji (Super+Period) are its screens.
- A calculator in the palette: units that carry through sums, conversions, currencies at
  the European Central Bank's daily rates, percentages, number bases, the time in a city
  and dates ("days until 25 dec", "today + 3 weeks"); Calculator History keeps what you copied.
- Search Files: names under the folders you choose (plocate's database when there is
  one), recently used files with nothing typed, a type filter and a preview.
- Quicklinks, snippets and custom commands, written in Settings, Launcher: addresses and
  commands with {argument}, {clipboard} and {date} filled in, asked for beside the search.
  A snippet's keyword typed in any app becomes the snippet (Expand snippet keywords).
- Window commands: halves (again for thirds), quarters, thirds, fourths, sixths, center,
  almost maximize, restore, the next screen, sizes of your own; window layouts put a saved
  arrangement back and start the apps missing from it. Any palette entry takes an alias
  and a global shortcut; an app's shortcut hides it again when it's in front.
- Notes: Markdown notes, a file each in ~/Documents/Notes, drawn as they are written and
  saved as they are typed; New Note and Search Notes from the palette.
- System actions from the palette: sleep, restart, shut down, log out, quit all apps,
  empty the Trash, light and dark, Bluetooth, Wi-Fi, mute, night light, Do Not Disturb,
  eject all disks; one that happens out of sight says so in a pill.
- Phone clipboard (Settings > Bluetooth): while a paired phone is connected, the two share
  the clipboard, text and pictures. On connecting each side gets the other's last five
  copies (in the Super+V history) and the newest becomes both clipboards; after that every
  copy on either side goes to the other. Rooted phones need the atrium-clipsync KernelSU
  module, linked from the page.
- A focus ring on tiles and secret windows (the accent, as caelestia's border), fading
  from one to the next, and focus that follows the pointer among them.
- Super + scroll steps through spaces; with Alt the focused window comes along.
- atrium-askpass: sudo -A and ssh ask for passwords in the shell's own card, and the
  session points SUDO_ASKPASS and SSH_ASKPASS at it unless they're already set.
- Shell actions for the Notification Center, Control Center and clearing notifications,
  for key bindings.
- HDR, as Windows does it: an HDR switch per screen in Settings, Displays (only on screens
  whose EDID says they take HDR10) and an SDR brightness slider (0-100, 80-480 nits, like
  Windows' "SDR content brightness"). The screen gets an HDR10 signal described with its
  own peak brightness; everything else looks as it did, as bright as the slider sets, while
  HDR games and videos show their full brightness. Screenshots and screen sharing of an HDR
  screen come out as they would without HDR, and Night Light warms it without bending its
  brightness. Apps can say their content is HDR (color management).
- Software Update in Settings, for atrium itself, the system and the AUR: a new atrium
  release is downloaded from GitHub and installed for the next login while this session
  keeps running; the system updates through PackageKit, or pacman itself where PackageKit
  isn't installed; AUR packages update through paru or yay in a terminal. What's newer,
  Check Now and Update Now with progress, a note when a restart finishes it, a menu bar
  badge while updates wait, and a check at login and every six hours.
- The package recommends a terminal, file manager and text editor (Ghostty, Dolphin, Kate)
  instead of atrium shipping its own.
- Variable refresh (FreeSync, G-Sync Compatible) per screen in Settings, Displays: off,
  always, or only while a fullscreen game is in front (the default), remembered per screen.
- Night Light: warmer colours from sunset to sunrise (worked out from the time zone, no
  location needed), between set hours, or always, as warm as Settings, Displays says; the
  Control Center turns it on or off until the schedule next changes. An app that sets the
  screen's gamma itself (gammastep, wlsunset) keeps that screen.
- Screenshots: Print opens a bar to take the whole screen, a window or a selection (or
  start recording); Shift+Print takes the screen, Alt+Print a window, Super+Shift+S a
  selection (Space switches to windows). The shot lands in Pictures/Screenshots and on the
  clipboard, and waits in the corner: click it to add arrows, boxes and blur, or crop.
  Apps asking the portal for a screenshot or a colour get the same tool.
- When an app asks to share the screen, atrium asks which screen or window, with a picture
  of each; a shared window that isn't changing still shows up for the people watching.
- Apps can set the wallpaper (atrium keeps its own copy of the picture) and keep the
  computer from idling (seen by logind, as `systemd-inhibit --list` shows) through the portal.
- When an app asks the portal for permission (setting the wallpaper, running in the
  background), atrium asks in its own dialog, with the app's icon and any choices.
- atrium's log goes to the journal (`journalctl -t atrium`) when a display manager
  starts it, instead of being lost on the console.
- Apps start in a systemd scope named for them, so portals know which app is asking
  (global shortcuts need that) and systemd keeps each app's processes together.
- Apps that ask the portal follow atrium's dark or light and accent colour, live.
- Apps' global shortcuts through the portal (Discord's push-to-talk, OBS): atrium's own
  portal backend binds them, tells the app when they're pressed and let go, and lists them
  in Settings, Keyboard Shortcuts to change.
- Settings, Printers: CUPS printers and the network's own, the default, and the queue;
  says to install cups where it's missing.
- Settings, Notifications: each app's notifications on, quiet (straight to the notification
  center) or off.
- Settings, Appearance: icon theme, font, monospace font and size, and the cursor theme,
  picked from what's installed and applied to GTK and Qt apps together.
- USB drives and memory cards mount when plugged in, with a notice to open them; an eject
  menu in the menu bar; Settings, Disks mounts, unmounts and ejects.
- On laptops, the battery in the menu bar, a notice when it runs low, and in Settings,
  Power, what's left, a charge limit where the hardware has one, and the lid's action.
- Settings, Default Apps: the browser, mail, files, text, images, video, music, PDF and
  terminal apps, as every app and xdg-open read them.
- Settings, Login Items: what opens at login, the system's turned off and on, more added.
- Settings, Date & Time: network time and the time zone (asking for the administrator's
  password), and a 24-hour menu bar clock; the clock follows a new zone at once.
- Settings, Language & Region: the system's language and formats, with a sample.
- Keyboard layouts as macOS's Input Sources: add from every xkeyboard-config layout and
  variant, reorder and remove in Settings, Keyboard; the menu bar shows the one in use and
  switches between them. Switch keys, a compose key, and a layout per window.
- A welcome at the first login: light or dark, accent, Liquid Glass, keyboard layout,
  web browser and terminal, and bringing along the wallpaper from Plasma, GNOME,
  Hyprland (hyprpaper), waypaper or caelestia. Skippable, and again from the launcher.
- A wallpaper, chosen in Settings, Appearance, filling, fitting, stretching, centring or
  tiling the picture on each screen.
- Settings says when something it relies on isn't there (NetworkManager, BlueZ or a
  Bluetooth adapter, PipeWire, AccountsService, cliphist, ddcutil, gpu-screen-recorder)
  instead of showing controls that do nothing, and picks up a service that starts later.
- A PKGBUILD (packaging/arch) builds and installs atrium with everything it needs:
  `makepkg -si`.
- Apps set to start at login start with atrium, and user services made for a graphical
  session run with it; both stop when you log out (atrium-session.target).
- After a crash, the next login says so, with where to find the details.
- A shell that keeps crashing comes back in safe mode (no effects), and is never given up on.
- Settings are copied before an update changes how they're stored.
- Icons show on systems whose Qt names no icon theme: the shell picks an installed one.
- Apps can ask for blur behind their translucent parts (ext-background-effect; foot's
  `blur` option, for one) when transparent windows are on.
- The bar's space highlight turns whatever it covers into its own colours as it slides,
  crisply, even halfway across a number or an icon (caelestia's effect).
- Control Center: the media tile grows into Now Playing, with the artwork large, a
  scrubber to seek, the controls, and the other players to switch between.
- Login sessions for the display manager: "atrium" and "atrium (uwsm-managed)".
- Portals for the atrium session: GTK's backend for the file chooser and settings (dark
  mode, title-bar buttons), screen sharing and screenshots through the wlroots capture.
- Users & Groups: Change Password… (checked against the current one, with PAM's reason
  when a new password is turned down) and, for admins, Add User… (the account name is
  filled in from the full name; optionally an admin).
- Light and dark appearance (Settings → Appearance): the bar, panels, Dock, Settings, title
  bars and atrium's GTK theme switch together, and apps that follow the system colour
  scheme (libadwaita, Firefox, Chromium, Qt) switch live.
- Accent colour (Settings → Appearance), macOS's set as swatches: the shell's highlights,
  Mission Control's selection, atrium's GTK theme and GNOME's accent-color for libadwaita apps.
  Multicolour keeps the default palette.
- A login screen for greetd (`atrium --greeter`, example config in share/atrium): the time,
  the people who can log in, a password field, Sleep/Restart/Shut Down and which desktop to
  start, with the last login preselected (left/right to pick someone else). No shortcut
  works there.
- Emoji picker (Super+Period): every emoji your emoji font can draw, by group and searchable by
  name; the one you pick is typed where you were typing, or copied when there's no text field.
- Liquid Glass (Settings → Appearance, off by default), after Apple's: the bar's pills, the
  Dock and the panels are clear lenses whose rounded rim bends what is behind, with a thin
  line of light along the rim coloured by what is behind it, a soft shadow, and just enough
  tint to keep them legible. Clear or Tinted (more opaque), as in macOS 26.1; the glass
  comes in by bending light rather than fading. The shell tells atrium each panel's exact
  shape, so the glass's edges are smooth at any size.
- Qt apps take atrium's colours (dark or light, with the accent) through the qtengine
  platform theme, live: a generated KDE colour scheme, keeping your own qtengine style,
  icons and fonts. Without qtengine, plasma-integration's platform theme gets them
  through a kdeglobals below your own; with neither, Qt's portal theme still follows dark and
  light. fcitx5's candidate popup follows dark mode and the accent colour.
- Settings: Users & Groups: your picture (click to choose another), full name, account
  name and whether you're an admin, and the other people with an account here, through
  AccountsService.
- Keys as in caelestia: Super+arrows focus the window that way, Super+Shift+arrows move
  it (tiled: trade places; floating: snap to that half, up maximizes), Ctrl+Super+Shift+←/→
  take it to the space before or after, Super+0 is space 10, Super+Page Up/Down step
  through spaces, Super+Alt+F maximizes, Super+Alt+Space floats a window out of the tiles,
  Super+P shows it on every space. Registries from before get the new keys; shortcuts
  the user changed stay as they are.
- Tiling (Super+\): the current space tiles its windows dwindle-style inside the same
  frame as a secret space, over a dimmed, frosted desktop. New windows take a slot, closing
  one closes the gap, dragging a window onto another swaps them, and turning it off puts
  every window back where it was. The Dock hides and the bar steps aside to its space pill
  at the top center (the pointer at the top brings the whole bar back). A secret space
  still shows over it.
- The margin around secret-space and tiled windows is the same on every side.
- Screens plugged in and out: a window moved onto another screen joins the space showing
  there, and an unplugged screen's windows come back. `atriumctl output create` adds a
  virtual screen (headless, or another window when nested) and `output remove` takes one
  away.
- X11 apps: windows open where the user asked (`-geometry`) or where the app asks on
  first launch; splash screens are centered and undecorated and don't take focus;
  notifications, menus and tooltips that are ordinary windows stay undecorated, unfocused
  and out of the Dock; "always on top", "always below", "on every space" (sticky), "skip
  taskbar" and "demands attention" work.
- A window's dialogs rise with it.
- Input methods (fcitx5, ibus and others over input-method-v2 / text-input-v3): they get
  the keys while a text field is focused, what they compose lands in the app, and their
  candidate popup sits under the text cursor.
- `atriumctl layers` lists panels, docks and overlays (layer surfaces).
- Sandboxed apps (Flatpak, over security-context) can't see the screen, other windows or
  the clipboard, fake input, or change the desktop and its displays.
- Modal dialogs keep focus: clicking or switching to the window behind one brings up the
  dialog instead.
- Windows' own icons (xdg-toplevel-icon) show in the Dock and the launcher for apps
  without a desktop entry; window tags and content types are listed over IPC.
- "Allow tearing in games" (Displays): fullscreen games that ask for it skip waiting for
  the screen's refresh. Off by default.
- Keyboard Shortcuts warns when two shortcuts use the same keys (however they are
  written, and counting what Mod stands for) and names the other one.
- Settings pages for Wi-Fi & Network (join with a password, forget, disconnect), Bluetooth
  (connect, forget, pair nearby devices), Sound (output and input devices, their volume,
  each app's volume) and Displays (arrangement by dragging, resolution, refresh rate,
  scale, rotation, on/off). Each monitor is remembered by make, model and serial and set
  up the same way when plugged in again. The Control Center links to them.
- System Settings (Super+Comma, or the logo menu): a sidebar of pages as on macOS, with
  search across every setting, a control per setting and a reset for changed ones; an
  Apps page for where each app opens, starting it with its secret space, its Dock pin
  and how it opens; a Shortcuts editor that records keys (atrium lets them through while
  it listens); window rules; About. It runs as its own app with its own name and icon.
- keyboard-shortcuts-inhibit: a focused client that asks gets the keys atrium would take
  (VMs, remote desktops); switching VTs stays atrium's.
- The registry: one SQLite database atrium owns ($XDG_CONFIG_HOME/atrium/registry.db)
  holding typed settings, a record per app (the space it opens in, whether showing that
  space starts it, its Dock pin, its remembered window), pattern rules and shortcuts, each
  changed through its own IPC calls and `atriumctl apps|app|dock|rules|rule|shortcuts|
  shortcut`. The old settings.json and placements.json are imported once.
- An empty Dock stays out of the way instead of showing an empty shelf.
- Secret spaces open their windows semi-fullscreen: large, centered, with a margin of
  blurred desktop around them (`windows.secret_margin`), no drop shadow, and their old
  size back when they leave. A window rule can `launch` its app: showing the secret space
  starts rule apps that aren't running and pulls their windows in from elsewhere, so the
  space's hotkey opens the app and closes the space again, as caelestia's toggle does.
- Dock pins rearrange by dragging: drag an app among the pins to move it, a running app
  into them to pin it, or a pin up off the Dock to remove it.
- Secret spaces in the menu bar: a chip for each that holds windows, with its apps; click
  to show it.
- Network status in the menu bar: wired or Wi-Fi strength, download and upload speed (averaged over a few seconds) over
  the real interfaces (`bar.net_speed`), and Bluetooth while a device is connected; opens
  the Control Center.
- Polkit agent in the shell: apps asking for administrator rights get a password dialog
  (app icon with a lock, whose password, shake on a wrong one); it steps aside when the
  session already has an agent. A password that worked is remembered for five minutes,
  as sudo does (`security.remember_admin`, on by default).
- The distribution's logo at the bar's left end opens a menu like the Apple menu: About
  This Computer (OS, processor, graphics, installed memory, kernel, uptime, all read from
  the hardware), Sleep, Restart, Shut Down and Log Out; the last three ask first and go
  ahead after a minute, as macOS does.
- Media keys: volume, microphone mute, brightness and playback, also on the lock screen
  (keybinds take `"locked": true`). Volume and brightness move in macOS's 16 steps (Shift
  for quarter steps), never past 100%, and show a moment under the bar's right end.
- Control Center modules expand like macOS: the tile grows into the whole panel with its
  own switch, known and other networks or devices, a password field for new secured
  Wi-Fi networks, and pairing of nearby Bluetooth devices; the bar button steps back.
- Launcher results, the Dock's app list and the desktop's files, selection and actions are
  native models in the shell's C++ plugin; the desktop watches its folder directly and
  keeps icons (and their animations) for files that stay.
- Compositor core ported from dwl to C++23 on wlroots 0.20 + scenefx 0.5: outputs, input,
  xdg-shell, layer-shell, Xwayland, session lock, output management/power, gamma, idle
  inhibit, pointer constraints, relative pointer, cursor shape, virtual keyboard/pointer,
  drag and drop, clipboard and primary selection, screencopy and image-capture.
- Floating window management: centered and cascaded placement, dialogs centered on their
  parent, click to focus and raise, maximize, fullscreen, minimize, focus cycling.
- Interactive move and resize from client title bars and edges, and Mod + left/right drag;
  snapping to screen edges; dragging a maximized window restores it under the cursor.
- Rounded corners on the visible window corners, including client title-bar subsurfaces,
  and focus-aware soft shadows.
- wlr-foreign-toplevel-management and ext-foreign-toplevel-list handles, with activate,
  minimize, maximize, fullscreen and close from docks; per-window capture sources.
- Built-in keybinds for terminal, close, fullscreen, maximize, minimize, focus cycling, quit
  and VT switching; Alt as the modifier when running nested.
- `--version`, `--debug`, `--startup`; version from `scripts/version.sh`.
- Settings store: 30 typed settings (appearance, windows, keyboard, mouse/touchpad, cursor,
  power, shortcuts) with schema for the Settings app, validation, live apply, only changed
  values persisted to `$XDG_CONFIG_HOME/atrium/settings.json` (`--settings` to override).
- Keybinds as a setting (`"Mod+Shift+E"` chords, named actions); Ctrl+Alt+F1-F12 VT switching.
- Control socket (JSON lines): windows, outputs, settings get/set/reset/schema, actions,
  per-window focus/close/minimize/maximize/fullscreen/move/resize, subscribe to window and
  settings events; `atriumctl` CLI.
- Title bars drawn by atrium on every window that allows it (xdg-decoration forced
  server-side, KDE server-decoration, X11 without Motif no-title): round minimize /
  maximize / close buttons at the top right with hover glyphs, centered title, press and
  backdrop states, double-click to zoom, drag to move; resize bands around the frame.
- A hairline outline around windows so dark windows stay distinct on a dark desktop.
- An `atrium` GTK theme installed and selected for the session so libadwaita header bars
  look like atrium's title bars; GNOME button layout set to match in real sessions.
- Dragging a maximized window restores it only once the pointer really moves.
- Spaces: numbered per output, created on demand and removed when empty and left
  (Mod+1-9, Mod+Shift+1-9 to move a window, Mod+Ctrl+Left/Right to step); focusing a window
  on another space switches to it; ext-workspace-v1 for panels.
- Secret spaces: named overlays on a dimmed backdrop over the current space (Mod+D for
  "communication", Mod+Shift+D to send a window in or back out); click the backdrop to put
  one away; it hides itself when its last window leaves.
- Window rules setting (`windows.rules`): app id / title regexes → space, secret space,
  maximized, fullscreen; default sends Discord/Equibop/Vesktop/WhatsApp to "communication".
- IPC and atriumctl: `spaces`, `space N`, `secret NAME`, `send ID N|NAME`, space events.
- Animations: windows fade and rise in on open, fade out on close (from a snapshot),
  minimize and restore, maximize; spaces slide on switch; secret spaces fade with their
  backdrop. `appearance.animations` and `appearance.animation_speed`.
- Blur: frosted wallpaper behind translucent windows (scenefx optimized blur), and the whole
  screen blurred behind a secret space's backdrop; `appearance.blur`, radius and passes.
- Snapping: drag a window to a screen edge for a half, a corner for a quarter, the top edge
  to maximize, with a frosted preview; Mod+Left/Right snap, Mod+Down restores; snapped
  windows restore their size when dragged away; `windows.snapping` and `windows.snap_gap`.
- Overview (Mod+Ctrl+Up): every window of the shown spaces scaled out side by side over a
  frosted desktop, live, keeping their arrangement; hover or arrow keys highlight one and
  show its title, click or Return goes to it, Escape or a click on the desktop goes back.
- Window title bars now stay in the close animation.
- `appearance.transparency` (off by default): windows get a solid background so
  translucent apps draw opaque; on, they show the frosted desktop behind them.
- `windows.tiled_titlebars`: snapped windows can drop their title bar and use the room.
- Overview spaces strip: mini desktops of every space along the top; click one to look at
  it, drag a window thumbnail onto one to move the window there.
- Drag a window hard against the left or right end of the screens and it rides along to the
  space that way; the window holds still while the spaces slide.
- Window menu: right-click a title bar (or a GTK header bar) for Minimize, Zoom, Full Screen,
  Show on All Spaces, moving to the space before or after, and Close.
- The window stays active under the shell's menus and panels (title bar, the bar's app name), as on a Mac.
- Apps that ask for their windows back get them where they were, across restarts
  (xdg-session-management-v1), kept in the registry.
- Tabs torn out of a browser window drag their new window along (xdg-toplevel-drag-v1).
- Mouse & Touchpad per device: each mouse or touchpad can have its own speed, acceleration,
  scrolling direction and handedness over the shared settings (`atriumctl devices`, `device`).
- One Dock, as on a Mac: on the first screen, moving to another when the pointer rests at
  that screen's bottom edge (`dock.every_screen` puts one on each).
- An app's first window reopens on the screen it was last on, when that screen is still there.
- Fullscreen apps can get a space of their own, as on a Mac (`windows.fullscreen_space`, off by
  default): the window moves to a new space beside its own and comes back when it leaves
  fullscreen or closes.
- App exposé (Mod+Ctrl+Down, or "Show All Windows" in the Dock menu): the overview with only
  one app's windows; `app-expose` takes an app id.
- Window switcher (Alt+Tab / Alt+Shift+Tab): hold Alt and tap Tab to walk the space's
  windows most recently used first, with live previews after a moment; let go to switch,
  Escape cancels. Super+Tab / Super+Shift+Tab cycle through existing spaces.
- Windows reopen where they were: an app's first window comes back at the size, position,
  and maximized/snapped state it had when it last closed (`windows.remember_placement`).
- Desktop shell (Quickshell, `shell/`), started and kept running by atrium
  (`session.shell`: builtin, a command, or none; restarted with backoff if it crashes;
  `restart-shell` action).
- Top bar: caelestia-style space pills (five at a time, paging on; each space shows its
  apps' icons; the active pill slides; click to go, scroll to step), the focused app's icon,
  name and title, tray, volume (scroll, click to mute), date and time.
- Dock: pinned apps (`dock.pinned`), then running ones behind a divider; running dots, the
  front app's dot wider; click to launch (the icon bounces until it opens) or focus, again
  to step through its windows; names on hover; right-click for New Window, Keep in Dock /
  Remove from Dock, Quit. `dock.autohide`, `dock.magnify`.
- Bar and Dock hide over a fullscreen app and slide in when the pointer reaches the top or
  bottom edge.
- Frosted panels: with transparency on, the bar and Dock turn to glass and atrium blurs
  behind them only where they draw (`appearance.blurred_panels`).
- Desktop icons: the XDG desktop folder on the desktop, from the top left; thumbnails for
  pictures, launchers (Steam shortcuts) show their own icon and name; double-click opens;
  drag files out to apps or drop them in; right-click for Open, Rename (inline), Copy Path,
  Move to Trash, and on the desktop New Folder, Open Terminal Here, Open in Files
  (`desktop.icons`).
- A panel or desktop below the windows that asks for the keyboard gets it.
- KDE apps find their apps (Dolphin "Open With", default apps): atrium sets
  `XDG_MENU_PREFIX` when only the Plasma menu exists, and in a real session hands its
  environment to systemd and D-Bus.
- Rename on the desktop happens in place, on the label itself.
- Spotlight launcher (Super+Space): apps ranked by name, generic name, keywords and how
  often you open them; open windows to switch to; arithmetic (Enter copies the result);
  `>command` runs anything.
- `shell` action: shortcuts can open shell features (`shell launcher`).
- `Atrium` QML module (C++): the shell's logic (search ranking, calculator) is native.
- The shell's connection to atrium is native (C++): events are applied as they arrive
  instead of re-reading everything, so the bar and Dock react at once.
- Apps that elevate with pkexec (GParted) open: atrium lets local root into Xwayland (the
  `xhost +si:localuser:root` rule, no xhost or exports needed). Xwayland now starts with
  the session, and the startup command waits for it.
- 12-hour clock in the bar.
- Notifications: atrium's own server. Popups at the top right for 5 seconds (the app's
  timeout if it sets one), paused while hovered; swipe sideways or middle-click to dismiss;
  drag down or the chevron to see the whole text and the app's buttons; critical ones stay.
- Bell in the bar (dot for unread, muted under Do Not Disturb) opening the notification
  center: history grouped by app, dismiss one or a whole app, Clear All, Do Not Disturb
  (`notifications.dnd`). History survives restarts.
- Clipboard history (Super+V): what you copied, text and pictures, searchable, with the
  whole text or the full picture previewed beside the list; click or Enter copies it back,
  Delete forgets it, Clear All. Kept by cliphist (your existing history carries over);
  atrium runs its watchers (`session.clipboard_history`).
- Control Center (the sliders icon in the bar): Wi-Fi and Bluetooth with their networks and
  paired devices, Do Not Disturb, power mode, screen recording, display brightness (DDC/CI
  monitors and laptop panels), sound with the output device, and what's playing.
- Screen recording (gpu-screen-recorder, into ~/Videos/Recordings) with a red timer in the
  bar that stops it; `recording.audio` adds the speakers' sound.
- `power.profile` (default performance) and `displays.brightness` (default 100): applied at
  login, since monitors forget their brightness on boot.
- Panels no longer close themselves when clicked.
