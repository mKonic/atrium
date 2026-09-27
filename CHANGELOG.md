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
- Shake the pointer to find it: it grows for a moment.
- Frosted blur behind menus and input-method popups when transparency is on.
- A screen shader setting: a GLSL file drawn over every screen.
- Settings for the blur's brightness, contrast, vibrancy and grain, and Liquid Glass's tint, refraction, highlight and shadow.

### Changed
- Windows zoom in as they open and out as they close, and shrink into their app's Dock icon when minimised.
- Updates no longer show in the menu bar; Software Update in Settings has them.
- atrium draws with its own renderer: a full redraw with blur and Liquid Glass takes about half the time.

### Fixed
- The shell's fonts and icons missing on a system without caelestia's packages.
- A window launched or switched to over a fullscreen app staying hidden behind it.
- Alt+Tab's miniature of a window playing video flickering at its edges.
- The shell's text drawn in Rubik Light at every weight, with bold faked, since caelestia's fonts left.

## [v0.1.0] - 2026-09-27
### Added
- atrium.
