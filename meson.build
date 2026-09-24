project('atrium', 'cpp', 'c',
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
# atrium's own scenefx (subprojects/packagefiles/scenefx: refraction for
# Liquid Glass), built in and linked statically.
scenefx_proj = subproject('scenefx', default_options: ['examples=false', 'default_library=static'])
scenefx = scenefx_proj.get_variable('scenefx')
# The system's wlroots, else the pinned one (subprojects/wlroots.wrap).
wlroots = dependency('wlroots-0.20', version: '>=0.20.0')
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
libdrm = dependency('libdrm')
gio = dependency('gio-2.0')
sqlite = dependency('sqlite3')
json = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])

have_xwayland = wlroots.get_variable(pkgconfig: 'have_xwayland', default_value: 'false') == 'true'
xwayland_opt = get_option('xwayland')
if xwayland_opt.enabled() and not have_xwayland
  error('xwayland requested but wlroots was built without it')
endif
use_xwayland = have_xwayland and not xwayland_opt.disabled()

deps = [scenefx, wlroots, wayland_server, xkbcommon, libinput, pixman, json, sqlite, cairo, pangocairo, libdrm, gio]
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

# Where the shell lives: installed under datadir, or the source tree when
# atrium runs straight from the build directory.
paths = configuration_data()
paths.set_quoted('ATRIUM_DATADIR', get_option('prefix') / get_option('datadir') / 'atrium')
paths.set_quoted('ATRIUM_SOURCE_DIR', meson.project_source_root())
paths.set_quoted('ATRIUM_BUILD_DIR', meson.project_build_root())
paths.set_quoted('ATRIUM_BINDIR', get_option('prefix') / get_option('bindir'))
paths.set_quoted('ATRIUM_QML_DIR', get_option('prefix') / get_option('libdir') / 'atrium' / 'qml')
paths_h = configure_file(output: 'paths.hpp', configuration: paths)

install_subdir('shell', install_dir: get_option('datadir') / 'atrium', exclude_directories: ['plugin', 'host'])
# System Settings' desktop entry and icon.
install_subdir('data/share/applications', install_dir: get_option('datadir'), strip_directory: false)
install_subdir('data/share/icons', install_dir: get_option('datadir'), strip_directory: false)
install_data('data/share/xdg-desktop-portal/atrium-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
# Sessions for the display manager (plain, and under uwsm).
install_subdir('data/share/wayland-sessions', install_dir: get_option('datadir'), strip_directory: false)
# The login screen: an example greetd config (atrium --greeter), and where
# it remembers the last login.
install_data('data/lib/systemd/user/atrium-session.target', install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
install_data('data/lib/tmpfiles.d/atrium-greeter.conf', install_dir: get_option('prefix') / 'lib' / 'tmpfiles.d')
install_data('data/share/atrium/greetd-config.toml', install_dir: get_option('datadir') / 'atrium')
subdir('shell/plugin/Atrium')
subdir('shell/host')

subdir('src')
subdir('tests')
