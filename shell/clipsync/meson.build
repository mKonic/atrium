# atrium-clipsync: the clipboard shared with a phone over Bluetooth (see
# sync.hpp). The phone's side is the atrium-clipsync-ksu module.
clipsync_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'DBus', 'Network'])
clipsync_moc = qt6.compile_moc(headers: ['sync.hpp', 'wayland_clipboard.hpp', '../plugin/Atrium/bluetooth.hpp',
  '../plugin/Atrium/compositor.hpp'], dependencies: clipsync_qt)

clipsync_xml = wl_protocol_dir / 'staging/ext-data-control/ext-data-control-v1.xml'
clipsync_protocol = [
  custom_target('ext_data_control_client_h', input: clipsync_xml, output: 'ext-data-control-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('ext_data_control_client_c', input: clipsync_xml, output: 'ext-data-control-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('atrium-clipsync', 'main.cpp', 'sync.cpp', 'rfcomm.cpp', 'history.cpp', 'wayland_clipboard.cpp', 'clipsync_core.cpp',
  '../plugin/Atrium/bluetooth.cpp', '../plugin/Atrium/compositor.cpp', '../../src/accent.cpp', '../../src/palette.cpp',
  clipsync_moc, clipsync_protocol,
  dependencies: [clipsync_qt, atrium_runtime_dep, dependency('wayland-client'), dependency('bluez')],
  include_directories: include_directories('../plugin/Atrium', '../../src', '../..'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install_rpath: get_option('prefix') / get_option('libdir') / 'atrium',
  install: true,
)
