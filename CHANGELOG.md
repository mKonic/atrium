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
- Phone link: a paired phone's audio over the network, its media controls, and pairing, connecting and connecting automatically in Settings > Phone.
- Clicking a notification raises the app's own window.
- Selected text in the accent colour in every text field.
- A screen nothing was set for starts at a scale for its density.
- atrium's own keyring (atrium-keyring), the Secret Service browsers and mail keep passwords in: KWallet's are copied over the first time, it asks for the login password once per session, and an atrium crash doesn't lock it.
- atrium restarts itself after a crash, and apps that can reconnect (Qt) stay open.
- atrium's own open and save dialog, Open With and print dialog for apps that ask through portals (Flatpak and browsers), so xdg-desktop-portal-gtk is no longer needed.
- Sandboxed apps' notifications, account details, email and launchers through atrium's portal, and GNOME's settings (fonts, cursor, title-bar buttons) for GTK apps.
- The focused app's own menus in the menu bar (Qt 6, Qt 5 and X11 apps that publish them).
- The menu bar over a shown secret space, usable without putting it away.
- Shake the pointer to find it: it grows for a moment (Mouse & Touchpad: Shake to find the pointer).
- A lock screen (Super+L), which stays locked if it crashes and comes back.
- Unlock with a fingerprint, next to the password (fprintd).
- Lock Screen and Switch User in the system menu: someone else logs in through the login screen while your session stays open, locked.
- The screen locks before sleep (Power: Lock the screen before sleep), and when asked by `loginctl lock-session`.
- atrium-login, atrium's own login manager: the login screen, autologin, and back to the login screen after logging out (in place of SDDM or greetd). Logging in through it opens the keyring with the login password.
- The login screen remembers each person's desktop.
- The login screen has the keyboard layout, the battery and a Control Center with Wi-Fi, Bluetooth and brightness, and types Chinese, Japanese or Korean names with fcitx5.
- Dim the screen, turn it off, lock, or sleep after a while without input (Power; all Never unless you set them). A playing video holds them off.
- The power button asks before shutting down, and puts the computer to sleep from the lock screen (Power: When the power button is pressed).
- Log Out, Restart and Shut Down ask every app to quit first. Apps still open after a few seconds (unsaved work) are listed, as on Windows: answer them, Cancel, or go ahead anyway; doing nothing goes ahead after 30 seconds.
- X11 apps that save through a session manager (XSMP) are asked to save when you log out, and can ask about unsaved work; Cancel there cancels the logout.
- Reopen windows when logging back in, as on a Mac (in the Log Out dialog, and Session settings).
- What you copied stays on the clipboard after the app it came from quits, with every format it offered (rich text, HTML, pictures); a password manager's copies don't.
- Wobbly windows, off by default: windows wobble like jelly while dragged or resized and settle when let go, as KWin's do, with its five levels of wobbliness (Windows: Wobbly windows).
- Motion blur for moving windows, off by default (Windows: Motion blur).
- Blur materials, Hyprland's frost (crackled ice) and haze (a pearly sheen), in Appearance: Blur material.
- Settings for the blur's brightness, contrast, vibrancy and grain, and Liquid Glass's tint, refraction, highlight and shadow.
- Frosted blur behind menus and input-method popups when transparency is on, leaving their soft shadows clear.
- A screen shader setting, as Hyprland's: a GLSL fragment shader file drawn over every screen (Appearance: Screen shader).
- A color profile (ICC) per display, in Displays, applied as KWin does.
- An HDR calibration per display, in Displays: an ICC profile with an MHC2 tag, as Windows HDR Calibration and DisplayCAL make, with its measured peak.
- Closing a laptop's lid with another display on turns the built-in screen off, and opening it brings it back as it was.
- Displays on a second graphics card that can't take the main one's frames (no 3D of its own, a USB adapter) get them copied through the CPU instead of staying black.
- VR headsets go to VR apps (SteamVR, Monado) instead of joining the desktop.
- "Launch on …" another graphics card in an app's menu in the Dock and the launcher; apps that ask for the discrete GPU get it, as switcheroo-control would send them.
- Apps can ring a bell (a terminal on a Tab with nothing to complete): the sound theme's bell, and the window marked as wanting attention when it isn't in front (Windows: Alert sound).
- An app that stops responding has its windows dimmed, and a dialog offers to terminate it or wait (Windows: Not responding).
- Show Desktop (the show-desktop shortcut action): the windows move off past the screen corners until it is used again, a window is chosen or opened, or the space changes.
- Hot corners: pushing the pointer into a screen corner opens the overview, the app's windows, the desktop, the launcher, the notification center or a quick note, or locks the screen (Desktop: Top-left corner and the rest).
- A window with a modal dialog open over it is greyed and darkened until the dialog closes, as KWin's Dim Parent Window (Windows: Dim behind dialogs).
- Identify in Displays: each screen shows its number, name and resolution for a moment, as KDE's.
- A game controller counts as activity, so playing with one keeps the screen awake, as in KWin.
- Alt+Tab spreads the windows out with their titles above them, as a Mac's Mission Control, and they glide back with the one picked on top; Command-Tab's row of app icons and the row of small previews remain as choices (Windows: Alt+Tab shows).
- X11 apps are sharp on scaled screens: they draw at the screen's scale themselves, as with KDE's "Apply scaling themselves", or can be stretched instead (Displays: Older (X11) apps).
- atrium's Open, Save, Open With and Print dialogs belong to the app window that asked for them (xdg-foreign), as on GNOME and Plasma.
- Hide the pointer while typing, until the mouse moves (Mouse & Touchpad), and Num Lock on at start (Keyboard), as Hyprland has them.
- The Genie minimize effect, as on a Mac: a window pours into its app's Dock icon and back out (Windows: Minimize effect; Scale is the other).

### Changed
- Windows zoom in from a point as they open and out as they close, maximizing, snapping and restoring morph them into their new size, and Scale minimizes them into their app's Dock icon rather than the Dock's middle.
- A Dock icon's name shows above it, as on a Mac, instead of the Dock growing to fit it.
- Over fullscreen apps, the bar and the Dock come only for a window you made fullscreen (green button or shortcut), after holding the pointer at the edge for a moment; over a video or game that went fullscreen itself they stay away, and a tap of Super brings the bar.
- Clipboard history (Super+V) is kept by atrium itself, without cliphist or wl-clipboard, with cliphist's rules (750 entries, a copy's text and its picture each kept); what cliphist had is brought over the first time.

### Fixed
- A Dock menu with many rows cut off at the top (its app's name).
- The shell's fonts and icons missing on a system without caelestia's packages.
- A window launched or switched to over a fullscreen app staying hidden behind it.
- Every Rubik weight in the shell, not Light only.
- The calendar's today mark staying on the day the session started.
- The last notification popup vanishing instead of sliding out, and a notification updated after it left not popping up again.
- Apps hanging on their notifications (Discord) after something else took the notification service and left.
- A password prompt drawing a command over several lines on top of itself.
- A field's placeholder showing over text still being composed with an input method.
- A secret space staying shown, empty, after its last window closed.
- Gwenview closing on the next image: touchpad pinches and swipes now reach apps (pinch to zoom), and Gwenview's cleanup no longer meets a missing gestures protocol.
- Super+V's Clear All, lost when the launcher was rebuilt (Ctrl+Shift+Delete, or click it in the footer).

## [v0.1.0] - 2026-09-27
### Added
- atrium.
