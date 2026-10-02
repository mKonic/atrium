# The lock screen's Qt Wayland shell integration ("atrium-lock"): its windows
# are ext-session-lock-v1 lock surfaces (see lock.hpp).
lock_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'WaylandClient'], private_headers: true)
# QtWaylandClient's private headers include QtWaylandGlobal's, which no
# module brings in.
qt_headers = dependency('Qt6Core').get_variable(pkgconfig: 'includedir')
lock_qt_global = include_directories(qt_headers / 'QtWaylandGlobal' / lock_qt.version(),
  qt_headers / 'QtWaylandGlobal' / lock_qt.version() / 'QtWaylandGlobal')
qtwaylandscanner = find_program('qtwaylandscanner', dirs: ['/usr/lib/qt6', '/usr/lib/qt6/libexec', '/usr/libexec/qt6'])
lock_xml = wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml'
lock_protocol = [
  custom_target('lock_client_h', input: lock_xml, output: 'wayland-ext-session-lock-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('lock_client_c', input: lock_xml, output: 'ext-session-lock-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]
lock_qt_h = custom_target('lock_qt_h', input: lock_xml, output: 'qwayland-ext-session-lock-v1.h',
  command: [qtwaylandscanner, 'client-header', '@INPUT@'], capture: true)
lock_qt_cpp = custom_target('lock_qt_cpp', input: lock_xml, output: 'qwayland-ext-session-lock-v1.cpp',
  command: [qtwaylandscanner, 'client-code', '@INPUT@'], capture: true)
lock_moc = qt6.compile_moc(sources: 'lock.cpp', headers: 'lock.hpp', dependencies: lock_qt,
  include_directories: include_directories('.'))

atrium_lock = shared_module('atrium-lock', 'lock.cpp', lock_protocol, lock_qt_h, lock_qt_cpp, lock_moc,
  dependencies: [lock_qt, dependency('wayland-client')],
  include_directories: lock_qt_global,
  cpp_args: ['-DQT_PLUGIN'] + meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install: true,
  install_dir: get_option('prefix') / get_option('libdir') / 'qt6' / 'plugins' / 'wayland-shell-integration',
)

# Where atrium run from the build tree points QT_PLUGIN_PATH (Qt looks for
# the integration in a wayland-shell-integration directory).
custom_target('atrium_lock_dev_plugin', input: atrium_lock, output: 'libatrium-lock-dev.stamp',
  command: ['sh', '-c', 'mkdir -p "$1/plugins/wayland-shell-integration" && cp "$2" "$1/plugins/wayland-shell-integration/" && touch "$3"',
            'sh', meson.current_build_dir(), '@INPUT@', '@OUTPUT@'],
  build_by_default: true)
