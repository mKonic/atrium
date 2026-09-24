# The Atrium QML module the shell imports. It is built into this directory
# (next to its qmldir), so the parent directory is a QML import path both
# here in the build tree and where it is installed.
qt6 = import('qt6')
qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'DBus', 'Network', 'Qml', 'Quick', 'Widgets'])

plugin_moc = qt6.compile_moc(sources: 'plugin.cpp', headers: ['compositor.hpp', 'views.hpp', 'notifications.hpp', 'notification_server.hpp', 'clipboard.hpp', 'brightness.hpp', 'recorder.hpp', 'apps.hpp', 'desktop_files.hpp', 'session.hpp', 'greeter.hpp', 'mpris.hpp', 'tray.hpp', 'audio.hpp', 'bluetooth.hpp', 'network.hpp', 'keyed_model.hpp', 'sysinfo.hpp', 'netspeed.hpp', 'admin_cache.hpp', 'polkit_agent.hpp', 'settings_pages.hpp', 'accounts.hpp', 'requirements.hpp', 'wallpaper.hpp', 'default_apps.hpp', 'keyboard_layouts.hpp', 'welcome.hpp', 'emojis.hpp'], dependencies: qt)

atrium_qml_dir = get_option('libdir') / 'atrium' / 'qml' / 'Atrium'

subdir('Shell')

shared_module('atriumplugin',
  'plugin.cpp', 'search.cpp', 'compositor.cpp', 'views.cpp', 'notifications.cpp', 'notification_server.cpp', 'clipboard.cpp', 'brightness.cpp', 'recorder.cpp', 'apps.cpp', 'desktop_files.cpp', 'files.cpp', 'levels.cpp', 'session.cpp', 'greeter.cpp', 'mpris.cpp', 'tray.cpp', 'audio.cpp', 'bluetooth.cpp', 'network.cpp', 'keyed_model.cpp', 'sysinfo.cpp', 'sysinfo_core.cpp', 'netspeed.cpp', 'netspeed_core.cpp', 'admin_cache.cpp', 'polkit_agent.cpp', 'settings_pages.cpp', 'accounts.cpp', 'requirements.cpp', 'wallpaper.cpp', 'wallpaper_core.cpp', 'default_apps.cpp', 'mimeapps_core.cpp', 'keyboard_layouts.cpp', 'keyboard_core.cpp', 'welcome.cpp', '../../../src/keyboard_conf.cpp', 'passwd_core.cpp', 'emoji_core.cpp', 'emojis.cpp', '../../../src/accent.cpp', '../../../src/palette.cpp', plugin_moc, version_h,
  dependencies: [qt, atrium_runtime_dep, dependency('polkit-qt6-agent-1'), dependency('libpulse'), dependency('libpulse-mainloop-glib'), dependency('libxcrypt'), dependency('fontconfig')],
  include_directories: include_directories('../../../src'),
  # GCC 16 trips over Qt's own headers; nothing of ours.
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  # Loaded into atrium-shell, which has no sanitizer runtime to host it.
  override_options: ['b_sanitize=none'],
  install: true,
  install_dir: atrium_qml_dir,
  install_rpath: atrium_libdir,
)

configure_file(input: 'qmldir', output: 'qmldir', copy: true, install: true, install_dir: atrium_qml_dir)

search_core = files('search.cpp')
