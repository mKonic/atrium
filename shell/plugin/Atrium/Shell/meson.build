# The shell's runtime (windows, screens, installed apps, the clock), as a
# library both QML modules link: Atrium.Shell registers its types, and
# Atrium's services share its DesktopEntries.
shell_qt = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick'])
layershellqt = dependency('LayerShellQt', method: 'cmake', modules: ['LayerShellQt::Interface'])
atrium_libdir = get_option('prefix') / get_option('libdir') / 'atrium'

runtime_moc = qt6.compile_moc(headers: ['scope.hpp', 'screens.hpp', 'shell_api.hpp', 'variants.hpp', 'system_clock.hpp', 'windows.hpp', 'desktop_entries.hpp'], dependencies: shell_qt)

atrium_runtime = shared_library('atrium-shell-runtime',
  'screens.cpp', 'shell_api.cpp', 'variants.cpp', 'system_clock.cpp', 'windows.cpp', 'desktop_entries.cpp', 'desktop_entry_core.cpp', runtime_moc,
  dependencies: [shell_qt, layershellqt],
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
