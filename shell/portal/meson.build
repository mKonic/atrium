# atrium-portal: the xdg-desktop-portal backend (see main.cpp), sharing the
# shell plugin's IPC client and portal code.
portal_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'DBus', 'Network'])
portal_moc = qt6.compile_moc(headers: ['../plugin/Atrium/portal.hpp', '../plugin/Atrium/compositor.hpp'],
  dependencies: portal_qt)

executable('atrium-portal', 'main.cpp', '../plugin/Atrium/portal.cpp', '../plugin/Atrium/compositor.cpp',
  '../../src/accent.cpp', '../../src/palette.cpp', portal_moc, paths_h,
  dependencies: [portal_qt, atrium_runtime_dep],
  include_directories: include_directories('../plugin/Atrium', '../../src', '../..'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install_rpath: get_option('prefix') / get_option('libdir') / 'atrium',
  install: true,
)
