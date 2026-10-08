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

### Changed
- A Dock icon's name shows above it, as on a Mac, instead of the Dock growing to fit it.
- Over fullscreen apps, the bar and the Dock come only for a window you made fullscreen (green button or shortcut), after holding the pointer at the edge for a moment; over a video or game that went fullscreen itself they stay away, and a tap of Super brings the bar.

### Fixed
- The shell's fonts and icons missing on a system without caelestia's packages.
- A window launched or switched to over a fullscreen app staying hidden behind it.
- Every Rubik weight in the shell, not Light only.
- The calendar's today mark staying on the day the session started.
- The last notification popup vanishing instead of sliding out, and a notification updated after it left not popping up again.
- Apps hanging on their notifications (Discord) after something else took the notification service and left.
- A password prompt drawing a command over several lines on top of itself.
- A field's placeholder showing over text still being composed with an input method.
- A secret space staying shown, empty, after its last window closed.

## [v0.1.0] - 2026-09-27
### Added
- atrium.
