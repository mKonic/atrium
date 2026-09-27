<div align = center>

<img src="./.github/readme/header.svg" width="750" height="300" alt="banner">

<br>

[![Badge Release]][Actions]
[![Badge License]][License]
![Badge Language]
[![Badge Pull Requests]][Pull Requests]
[![Badge Issues]][Issues]
![Badge Hi Mom]<br>

<br>

atrium is a floating Wayland compositor that doesn't sacrifice on its looks.

It provides the latest Wayland features, has all the eyecandy and more...
<br>
<br>

</div>

# Features

- All of the eyecandy: rounded corners, shadows
- A lot of customization
- Easily expandable and readable codebase
- Fast and active development
- Not afraid to provide bleeding-edge features
- Settings applied instantly upon saving
- Floating/fullscreen windows
- Secret spaces (scratchpads)
- Powerful window rules
- Socket-based IPC
- Fully dynamic spaces

# Install

### From a release

Download `atrium-git-*.pkg.tar.zst` from [Releases], then:

```sh
sudo pacman -U atrium-git-*.pkg.tar.zst
```

### From source

```sh
git clone https://github.com/mKonic/atrium.git
cd atrium/packaging/arch
makepkg -si
```

<br>
<br>

<div align = center>

# Gallery

<br>

![Preview A]

<br>

![Preview B]

<br>

![Preview C]

<br>
<br>

</div>

# Special Thanks

<br>

**[wlroots]** - *For powering atrium*

**[tinywl]** - *For showing how 2 do stuff*

**[Sway]** - *For showing how 2 do stuff the overkill way*

**[dwl]** - *For showing how 2 do stuff the hacky way*


<!----------------------------------{ Thanks }--------------------------------->

[WlRoots]: https://gitlab.freedesktop.org/wlroots/wlroots
[TinyWl]: https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/tinywl/tinywl.c
[Sway]: https://github.com/swaywm/sway
[DWL]: https://codeberg.org/dwl/dwl

<!----------------------------------{ Links }---------------------------------->

[Releases]: https://github.com/mKonic/atrium/releases
[Actions]: https://github.com/mKonic/atrium/actions/workflows/release.yml
[License]: ./LICENSE
[Pull Requests]: https://github.com/mKonic/atrium/pulls
[Issues]: https://github.com/mKonic/atrium/issues

<!----------------------------------{ Images }--------------------------------->

[Preview A]: ./.github/readme/preview-1.png
[Preview B]: ./.github/readme/preview-2.png
[Preview C]: ./.github/readme/preview-3.png


<!----------------------------------{ Badges }--------------------------------->

[Badge Release]: https://img.shields.io/github/actions/workflow/status/mKonic/atrium/release.yml?logo=github&label=Release
[Badge License]: https://img.shields.io/badge/license-GPL--3.0-green
[Badge Language]: https://img.shields.io/github/languages/top/mKonic/atrium
[Badge Pull Requests]: https://img.shields.io/github/issues-pr/mKonic/atrium
[Badge Issues]: https://img.shields.io/github/issues/mKonic/atrium
[Badge Hi Mom]: https://img.shields.io/badge/Hi-mom!-ff69b4
