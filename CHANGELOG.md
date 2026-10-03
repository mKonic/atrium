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
### Added
- Wobbly windows, off by default (Windows: Wobbly windows).
- Motion blur for moving windows, off by default (Windows: Motion blur).
- The Genie minimize effect, as on a Mac (Windows: Minimize effect; Scale is the other).
- A color profile (ICC) per display, in Displays.
- An HDR calibration per display, in Displays: an ICC profile with an MHC2 tag, as Windows HDR Calibration and DisplayCAL make, with its measured peak brightness.
- Shake the pointer to find it: it grows for a moment, whatever it shows (an app's own cursor too).
- Frosted blur behind menus and input-method popups when transparency is on.
- A screen shader setting: a GLSL file drawn over every screen.
- Settings for the blur's brightness, contrast, vibrancy and grain, and Liquid Glass's tint, refraction, highlight and shadow.
- Touchpad swipes with three or four fingers: sideways to the next space, up for Mission Control, down for the app's windows.
- Touchscreens and drawing tablets.
- Closing a laptop's lid with another display connected turns the built-in screen off.
- Displays on a second graphics card: a laptop's discrete GPU, a dock, a DisplayLink adapter.
- VR headsets go to VR apps (SteamVR, Monado) instead of joining the desktop.
- "Launch on …" another graphics card in an app's menu in the Dock and the launcher; apps that ask for the discrete GPU get it.
- atrium-login, atrium's own login manager: the login screen, autologin, and back to the login screen after logging out (in place of SDDM or greetd).
- A lock screen (Super+L), which stays locked if it crashes and comes back.
- Lock Screen and Switch User in the system menu: someone else logs in while your session stays open (locked); logging in as you again goes straight back to it.
- The screen locks before sleep (Power: Lock the screen before sleep), and when asked by `loginctl lock-session`.
- The login screen remembers each person's desktop.
- The login screen has the keyboard layout, the battery and a Control Center with Wi-Fi, Bluetooth and brightness, and types Chinese, Japanese or Korean names with fcitx5.
- A screen nothing was set for starts at a scale that suits its pixel density.
- Unlock with a fingerprint, next to the password (fprintd).
- Dim the screen, turn it off, lock, or sleep after a while without input (Power; all Never unless you set them). A playing video holds them off.
- Log Out, Restart and Shut Down ask every app to quit first; an app that won't (unsaved work) cancels it and says which.
- X11 apps that save through a session manager (XSMP) are asked to save when you log out, and can ask about unsaved work; Cancel there cancels the logout.
- When atrium crashes it starts again by itself, and Qt apps (System Settings, KDE apps) stay open through it.
- Reopen windows when logging back in, as on a Mac (in the Log Out dialog, and Session settings).
- The power button asks before shutting down, and puts the computer to sleep from the lock screen (Power: When the power button is pressed).
- Phone audio: a paired phone's sound plays here over Wi-Fi (Settings: Phone), paired from the phones nearby, with Connect, Disconnect, Forget and connect automatically like Bluetooth. The phone needs the atrium module (rooted, KernelSU).
- The phone's music shows in atrium's media controls (play, pause, skip, seek, cover art), and the media keys drive it.
- What you copied stays on the clipboard after the app it came from quits.
- Remote control for apps that ask (remote-desktop servers such as RustDesk, KVM switches): once you allow it, they use the keyboard, mouse and touchscreen, and see the screen if they ask to.
- The menu bar shows the focused app's own menus (File, Edit, View…), as on a Mac, for Qt and KDE apps, with their shortcuts; moving along the titles opens each, and menus that don't fit are under ».
- One keyboard and mouse for several computers (Deskflow, Input Leap): the pointer goes off the edge of the screen onto the next computer once you allow it; Super+Escape brings it back at any time.

### Changed
- Saved passwords and apps' keys (Chrome's, VS Code's, Flatpak apps') are kept by atrium's own keyring, opened by the login password; the first login copies everything over from KWallet. KWallet is no longer needed.
- Screen recording is atrium's own (atrium-record, from atrium's own stream of the screen, encoded with NVENC, VA-API or x264); gpu-screen-recorder is no longer needed.
- Maximizing, snapping and restoring a window morph it smoothly into its new size.
- Windows zoom in as they open and out as they close, and pour into their app's Dock icon when minimized.
- Updates no longer show in the menu bar; Software Update in Settings has them.
- atrium draws with its own renderer: a full redraw with blur and Liquid Glass takes about half the time.
- atrium speaks Wayland to apps itself, and runs X11 windows with its own window manager.
- atrium drives the displays itself, on drivers with or without atomic modesetting.
- atrium no longer needs wlroots, and runs nested only inside a Wayland session (not X11).
- Clipboard history (Super+V) is kept by atrium itself, without cliphist or wl-clipboard, and never records a password manager's copies.
- Screenshots are taken by atrium itself, without grim; `atriumctl screenshot` takes one of a screen, a window or an area.
- Sharing the screen goes through atrium itself, without xdg-desktop-portal-wlr: a screen, a window or an area, at the screen's refresh rate, with the pointer drawn in or sent apart (OBS), remembered for apps that ask to.
- Open and Save dialogs, Open With and the Print dialog are atrium's own, for sandboxed apps (Flatpak) and the GTK and Electron apps that ask the portal; so are their notifications, GNOME's settings, the user's name and picture (after asking), a new email and web apps added to the launcher (after asking). xdg-desktop-portal-gtk is no longer needed.
- A notification an app updates after it left the screen pops up again.

### Fixed
- The shell's fonts and icons missing on a system without caelestia's packages.
- A window launched or switched to over a fullscreen app staying hidden behind it.
- Alt+Tab's miniature of a window playing video flickering at its edges.
- The shell's text drawn in Rubik Light at every weight, with bold faked, since caelestia's fonts left.

## [v0.1.0] - 2026-09-27
### Added
- atrium.
