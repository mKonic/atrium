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
- The Genie minimize effect, as on a Mac (Windows: Minimize effect; Scale is the other).
- A color profile (ICC) per display, in Displays.
- Shake the pointer to find it: it grows for a moment.
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
- Unlock with a fingerprint, next to the password (fprintd).
- Dim the screen, turn it off, lock, or sleep after a while without input (Power; all Never unless you set them). A playing video holds them off.
- Log Out, Restart and Shut Down ask every app to quit first; an app that won't (unsaved work) cancels it and says which.
- When atrium crashes it starts again by itself, and Qt apps (System Settings, KDE apps) stay open through it.
- Reopen windows when logging back in, as on a Mac (in the Log Out dialog, and Session settings).
- The power button asks before shutting down, and puts the computer to sleep from the lock screen (Power: When the power button is pressed).
- Phone audio: a paired phone's sound plays here over Wi-Fi (Settings: Phone), paired from the phones nearby, with Connect, Disconnect, Forget and connect automatically like Bluetooth. The phone needs the atrium module (rooted, KernelSU).
- The phone's music shows in atrium's media controls (play, pause, skip, seek, cover art), and the media keys drive it.

### Changed
- Maximizing, snapping and restoring a window morph it smoothly into its new size.
- Windows zoom in as they open and out as they close, and pour into their app's Dock icon when minimized.
- Updates no longer show in the menu bar; Software Update in Settings has them.
- atrium draws with its own renderer: a full redraw with blur and Liquid Glass takes about half the time.
- atrium speaks Wayland to apps itself, and runs X11 windows with its own window manager.
- atrium drives the displays itself, on drivers with or without atomic modesetting.
- atrium no longer needs wlroots, and runs nested only inside a Wayland session (not X11).

### Fixed
- The shell's fonts and icons missing on a system without caelestia's packages.
- A window launched or switched to over a fullscreen app staying hidden behind it.
- Alt+Tab's miniature of a window playing video flickering at its edges.
- The shell's text drawn in Rubik Light at every weight, with bold faked, since caelestia's fonts left.

## [v0.1.0] - 2026-09-27
### Added
- atrium.
