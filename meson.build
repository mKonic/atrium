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

# wlroots and scenefx are atrium's own forks (git submodules, branch atrium:
# github.com/mKonic/wlroots and mKonic/scenefx). wlroots is a shared library
# kept in $libdir/atrium (its exports are only its API: scenefx carries
# copies of some of its internals, which a static wlroots would clash with);
# scenefx is linked in. wlroots first: scenefx finds it through its override.
wlroots_proj = subproject('wlroots', default_options: ['examples=false', 'default_library=shared',
  'renderers=gles2', 'backends=drm,libinput,x11', 'allocators=gbm,udmabuf', 'werror=false'])
wlroots = wlroots_proj.get_variable('wlroots')
meson.add_install_script('scripts/install-wlroots.sh', wlroots_proj.get_variable('lib_wlr'),
  get_option('prefix') / get_option('libdir'))
# scenefx must come before wlroots among the dependencies: it re-exports the
# wlr_scene_* symbols with effect support, and the first library providing a
# symbol wins.
scenefx_proj = subproject('scenefx', default_options: ['examples=false', 'default_library=static', 'werror=false'])
scenefx = scenefx_proj.get_variable('scenefx')
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
libdrm = dependency('libdrm')
gio = dependency('gio-2.0')
display_info = dependency('libdisplay-info')
sqlite = dependency('sqlite3')
json = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])

have_xwayland = wlroots.get_variable('have_xwayland', default_value: 'false') == 'true'
xwayland_opt = get_option('xwayland')
if xwayland_opt.enabled() and not have_xwayland
  error('xwayland requested but wlroots was built without it')
endif
use_xwayland = have_xwayland and not xwayland_opt.disabled()

deps = [scenefx, wlroots, wayland_server, xkbcommon, libinput, pixman, json, sqlite, cairo, pangocairo, libdrm, gio, display_info, dependency('sm'), dependency('ice')]
if use_xwayland
  deps += [dependency('xcb'), dependency('xcb-icccm')]
  add_project_arguments('-DATRIUM_XWAYLAND', language: 'cpp')
endif
# Logging to the journal when a display manager starts the session.
libsystemd = dependency('libsystemd', required: false)
if libsystemd.found()
  deps += libsystemd
  add_project_arguments('-DATRIUM_JOURNAL', language: 'cpp')
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

install_subdir('shell', install_dir: get_option('datadir') / 'atrium', exclude_directories: ['plugin', 'host', 'portal', 'clipsync', 'phonelink', 'lock'])
# System Settings' desktop entry and icon.
install_subdir('data/share/applications', install_dir: get_option('datadir'), strip_directory: false)
install_subdir('data/share/icons', install_dir: get_option('datadir'), strip_directory: false)
install_data('data/share/xdg-desktop-portal/atrium-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
# atrium's own portal backend (atrium-portal), started by D-Bus.
install_data('data/share/xdg-desktop-portal/portals/atrium.portal',
  install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')
portal_service = configuration_data()
portal_service.set('BINDIR', get_option('prefix') / get_option('bindir'))
portal_service.set('DATADIR', get_option('prefix') / get_option('datadir'))
configure_file(input: 'data/share/dbus-1/services/org.freedesktop.impl.portal.desktop.atrium.service.in',
  output: 'org.freedesktop.impl.portal.desktop.atrium.service', configuration: portal_service,
  install_dir: get_option('datadir') / 'dbus-1' / 'services')
# Sharing the screen: xdg-desktop-portal-wlr asks atrium's chooser (share.qml).
subdir('data/etc/xdg/xdg-desktop-portal-wlr')
# Sessions for the display manager (plain, and under uwsm).
install_subdir('data/share/wayland-sessions', install_dir: get_option('datadir'), strip_directory: false)
# sudo -A and ssh ask for passwords through the shell (SUDO_ASKPASS).
install_data('data/bin/atrium-askpass', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
# The login screen: an example greetd config (atrium --greeter), and where
# it remembers the last login.
install_data('data/lib/systemd/user/atrium-session.target', install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user')
# The greeter's memory is only for machines using greetd (its `greeter` user
# comes with greetd): the config's comments say to install it.
# The shell's own font (Rubik), for every user and the greeter.
install_subdir('data/share/atrium/fonts', install_dir: get_option('datadir') / 'atrium')
install_data('data/share/atrium/greetd-config.toml', 'data/share/atrium/greeter-tmpfiles.conf', install_dir: get_option('datadir') / 'atrium')
subdir('shell/plugin/Atrium')
subdir('shell/host')
subdir('shell/portal')
subdir('shell/clipsync')
subdir('shell/phonelink')
subdir('shell/lock')

subdir('src')
subdir('keyring')
subdir('login')
subdir('tests')
