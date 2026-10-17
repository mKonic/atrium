# atrium-phonelink: a phone's audio over the network (see daemon.hpp). The
# phone's side is the Atrium Link module (atrium-link).
phonelink_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'DBus', 'Network'])
phonelink_moc = qt6.compile_moc(headers: ['daemon.hpp', 'mpris.hpp', '../plugin/Atrium/compositor.hpp'], dependencies: phonelink_qt)

phonelink_xml = meson.project_source_root() / 'protocols/virtual-keyboard-unstable-v1.xml'
phonelink_protocol = [
  custom_target('virtual_keyboard_client_h', input: phonelink_xml, output: 'virtual-keyboard-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('virtual_keyboard_client_c', input: phonelink_xml, output: 'virtual-keyboard-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

executable('atrium-phonelink', 'main.cpp', 'daemon.cpp', 'keys.cpp', 'mpris.cpp', 'playback.cpp', 'avahi_qt.cpp', 'link_core.cpp', 'crypto.cpp',
  'jitter.cpp', '../plugin/Atrium/compositor.cpp', '../plugin/Atrium/identify_core.cpp', '../../src/accent.cpp', '../../src/palette.cpp', '../../src/records.cpp',
  phonelink_moc, phonelink_protocol,
  dependencies: [phonelink_qt, atrium_runtime_dep, json, dependency('libcrypto'), dependency('libpipewire-0.3', version: '>=1.4'),
    dependency('avahi-client'), dependency('wayland-client'), xkbcommon],
  include_directories: include_directories('../plugin/Atrium', '../../src', '../..'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install_rpath: get_option('prefix') / get_option('libdir') / 'atrium',
  install: true,
)
