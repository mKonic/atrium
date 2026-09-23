project('atrium', 'cpp',
  version: 'git',
  license: 'GPL-3.0-or-later',
  meson_version: '>=1.3.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')
add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  # wlroots/scenefx headers use C idioms (designated initializers out of order,
  # anonymous structs in unions) that g++ accepts but warns about.
  cpp.get_supported_arguments(
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-Wno-pedantic',
    '-Wno-c99-designator',
  ),
  language: 'cpp',
)

# scenefx must come before wlroots: it re-exports the wlr_scene_* symbols with
# effect support, and the first library providing a symbol wins at load time.
scenefx = dependency('scenefx-0.5')
wlroots = dependency('wlroots-0.20')
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')

have_xwayland = wlroots.get_variable(pkgconfig: 'have_xwayland', default_value: 'false') == 'true'
xwayland_opt = get_option('xwayland')
if xwayland_opt.enabled() and not have_xwayland
  error('xwayland requested but wlroots was built without it')
endif
use_xwayland = have_xwayland and not xwayland_opt.disabled()

deps = [scenefx, wlroots, wayland_server, xkbcommon, libinput, pixman]
if use_xwayland
  deps += [dependency('xcb'), dependency('xcb-icccm')]
  add_project_arguments('-DATRIUM_XWAYLAND', language: 'cpp')
endif

subdir('protocols')

version_h = custom_target('version.hpp',
  output: 'version.hpp',
  command: [files('scripts/version.sh'), 'header', '@OUTPUT@'],
  build_always_stale: true,
)

subdir('src')
