# atrium-phonelink: a phone's audio over the network (see daemon.hpp). The
# phone's side is the atrium-clipsync-ksu module.
phonelink_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'DBus', 'Network'])
phonelink_moc = qt6.compile_moc(headers: ['daemon.hpp', '../plugin/Atrium/compositor.hpp'], dependencies: phonelink_qt)

executable('atrium-phonelink', 'main.cpp', 'daemon.cpp', 'playback.cpp', 'avahi_qt.cpp', 'link_core.cpp', 'crypto.cpp',
  'jitter.cpp', '../plugin/Atrium/compositor.cpp', '../../src/accent.cpp', '../../src/palette.cpp', '../../src/records.cpp',
  phonelink_moc,
  dependencies: [phonelink_qt, atrium_runtime_dep, json, dependency('libcrypto'), dependency('libpipewire-0.3'),
    dependency('avahi-client')],
  include_directories: include_directories('../plugin/Atrium', '../../src', '../..'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install_rpath: get_option('prefix') / get_option('libdir') / 'atrium',
  install: true,
)
