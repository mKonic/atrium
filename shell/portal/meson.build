# atrium-portal: the xdg-desktop-portal backend (see main.cpp), sharing the
# shell plugin's IPC client and portal code.
portal_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'DBus', 'Network'])
portal_moc = qt6.compile_moc(headers: ['../plugin/Atrium/portal.hpp', '../plugin/Atrium/portal_secret.hpp', '../plugin/Atrium/portal_extras.hpp', '../plugin/Atrium/portal_files.hpp', '../plugin/Atrium/portal_print.hpp', '../plugin/Atrium/compositor.hpp'],
  dependencies: portal_qt)

portal_moc += qt6.compile_moc(headers: ['screencast.hpp', 'cast_stream.hpp', 'wayland_link.hpp', 'remote.hpp'],
  include_directories: include_directories('../plugin/Atrium'),
  dependencies: [portal_qt, dependency('libpipewire-0.3')])

executable('atrium-portal', 'main.cpp', 'screencast.cpp', 'remote.cpp', 'ipc_link.cpp', 'cast_stream.cpp', 'cast_core.cpp', 'wayland_link.cpp', capture_client_protocols, '../plugin/Atrium/portal.cpp', '../plugin/Atrium/portal_secret.cpp', '../plugin/Atrium/portal_extras.cpp', '../plugin/Atrium/portal_files.cpp', '../plugin/Atrium/portal_print.cpp', '../plugin/Atrium/printers_core.cpp', '../plugin/Atrium/portal_core.cpp', '../plugin/Atrium/share_core.cpp', '../plugin/Atrium/compositor.cpp', '../plugin/Atrium/identify_core.cpp',
  '../../src/accent.cpp', '../../src/palette.cpp', '../../src/records.cpp', portal_moc, paths_h,
  dependencies: [portal_qt, atrium_runtime_dep, json, dependency('gio-2.0'), dependency('gio-unix-2.0'),
    dependency('libpipewire-0.3'), dependency('wayland-client'), dependency('gbm'), dependency('libdrm')],
  include_directories: include_directories('../plugin/Atrium', '../../src', '../..', '../../protocols'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install_rpath: get_option('prefix') / get_option('libdir') / 'atrium',
  install: true,
)
