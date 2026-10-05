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

### Fixed
- The shell's fonts and icons missing on a system without caelestia's packages.
- A window launched or switched to over a fullscreen app staying hidden behind it.
- Every Rubik weight in the shell, not Light only.
- The calendar's today mark staying on the day the session started.
- The last notification popup vanishing instead of sliding out, and a notification updated after it left not popping up again.
- Apps hanging on their notifications (Discord) after something else took the notification service and left.
- A password prompt drawing a command over several lines on top of itself.
- A field's placeholder showing over text still being composed with an input method.

## [v0.1.0] - 2026-09-27
### Added
- atrium.
