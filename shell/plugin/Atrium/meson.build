# The Atrium QML module the shell imports. It is built into this directory
# (next to its qmldir), so the parent directory is a QML import path both
# here in the build tree and where it is installed.
qt6 = import('qt6')
qt = dependency('qt6', modules: ['Core', 'DBus', 'Network', 'Qml'])

plugin_moc = qt6.compile_moc(sources: 'plugin.cpp', headers: ['compositor.hpp', 'views.hpp', 'notifications.hpp', 'clipboard.hpp', 'brightness.hpp', 'recorder.hpp', 'apps.hpp', 'desktop_files.hpp', 'session.hpp', 'sysinfo.hpp', 'netspeed.hpp'], dependencies: qt)

atrium_qml_dir = get_option('libdir') / 'atrium' / 'qml' / 'Atrium'

shared_module('atriumplugin',
  'plugin.cpp', 'search.cpp', 'compositor.cpp', 'views.cpp', 'notifications.cpp', 'clipboard.cpp', 'brightness.cpp', 'recorder.cpp', 'apps.cpp', 'desktop_files.cpp', 'files.cpp', 'levels.cpp', 'session.cpp', 'sysinfo.cpp', 'sysinfo_core.cpp', 'netspeed.cpp', 'netspeed_core.cpp', plugin_moc, version_h,
  dependencies: qt,
  # GCC 16 trips over Qt's own headers; nothing of ours.
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  install: true,
  install_dir: atrium_qml_dir,
)

configure_file(input: 'qmldir', output: 'qmldir', copy: true, install: true, install_dir: atrium_qml_dir)

search_core = files('search.cpp')
