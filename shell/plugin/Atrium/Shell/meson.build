# The shell's runtime (windows, screens, installed apps, the clock), as a
# library both QML modules link: Atrium.Shell registers its types, and
# Atrium's services share its DesktopEntries.
shell_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'Qml', 'Quick'])
# The window's wl_surface, for the protocols Qt has no API for.
shell_qt_private = dependency('qt6', version: '>=6.8', modules: ['Gui'], private_headers: true)
wayland_client = dependency('wayland-client')

client_protocols = []
foreach xml : [wl_protocol_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml']
  client_protocols += custom_target(xml.underscorify() + '_client_h', input: xml, output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  client_protocols += custom_target(xml.underscorify() + '_client_c', input: xml, output: '@BASENAME@-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach
# LayerShellQt ships CMake files only; without CMake, its library directly.
layershellqt = dependency('LayerShellQt', method: 'cmake', modules: ['LayerShellQt::Interface'], required: false)
if not layershellqt.found()
  layershellqt = meson.get_compiler('cpp').find_library('LayerShellQtInterface')
endif
atrium_libdir = get_option('prefix') / get_option('libdir') / 'atrium'

runtime_moc = qt6.compile_moc(headers: ['shortcut_inhibitor.hpp', 'scope.hpp', 'screens.hpp', 'shell_api.hpp', 'variants.hpp', 'system_clock.hpp', 'windows.hpp', 'desktop_entries.hpp'], dependencies: shell_qt)

atrium_runtime = shared_library('atrium-shell-runtime',
  'screens.cpp', 'shell_api.cpp', 'variants.cpp', 'system_clock.cpp', 'windows.cpp', 'desktop_entries.cpp', 'desktop_entry_core.cpp', 'shortcut_inhibitor.cpp', runtime_moc, client_protocols,
  dependencies: [shell_qt, shell_qt_private, layershellqt, wayland_client],
  include_directories: include_directories('../../../../src'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install: true,
  install_dir: get_option('libdir') / 'atrium',
)
atrium_runtime_dep = declare_dependency(link_with: atrium_runtime, include_directories: include_directories('.'), dependencies: shell_qt)

atrium_shell_qml_dir = atrium_qml_dir / 'Shell'
shell_plugin_moc = qt6.compile_moc(sources: 'plugin.cpp', dependencies: shell_qt)

shared_module('atriumshellplugin',
  'plugin.cpp', shell_plugin_moc,
  dependencies: atrium_runtime_dep,
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install: true,
  install_dir: atrium_shell_qml_dir,
  install_rpath: atrium_libdir,
)

foreach f : ['qmldir', 'IconImage.qml']
  configure_file(input: f, output: f, copy: true, install: true, install_dir: atrium_shell_qml_dir)
endforeach

desktop_entry_core = files('desktop_entry_core.cpp')
