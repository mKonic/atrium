# The Atrium QML module the shell imports. It is built into this directory
# (next to its qmldir), so the parent directory is a QML import path both
# here in the build tree and where it is installed.
qt6 = import('qt6')
qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'DBus', 'Network', 'Qml', 'Quick', 'Widgets'])

plugin_moc = qt6.compile_moc(sources: 'plugin.cpp', headers: ['compositor.hpp', 'views.hpp', 'notifications.hpp', 'notification_server.hpp', 'clipboard.hpp', 'phone_clipboard.hpp', 'phone_link.hpp', 'brightness.hpp', 'recorder.hpp', 'apps.hpp', 'launcher_model.hpp', 'file_search.hpp', 'calc_rates.hpp', 'notes.hpp', 'window_layouts.hpp', 'update_sources.hpp', 'desktop_files.hpp', 'folder_model.hpp', 'session.hpp', 'greeter.hpp', 'unlock.hpp', 'fingerprint.hpp', 'mpris.hpp', 'tray.hpp', 'app_menu.hpp', 'audio.hpp', 'bluetooth.hpp', 'network.hpp', 'keyed_model.hpp', 'sysinfo.hpp', 'netspeed.hpp', 'admin_cache.hpp', 'polkit_agent.hpp', 'settings_pages.hpp', 'accounts.hpp', 'requirements.hpp', 'wallpaper.hpp', 'default_apps.hpp', 'keyboard_layouts.hpp', 'welcome.hpp', 'share.hpp', 'file_chooser.hpp', 'app_chooser.hpp', 'print_prompt.hpp', 'access.hpp', 'capture.hpp', 'markup.hpp', 'updates.hpp', 'datetime.hpp', 'region.hpp', 'autostart.hpp', 'battery.hpp', 'disks.hpp', 'themes.hpp', 'printers.hpp', 'emojis.hpp'], dependencies: qt)

atrium_qml_dir = get_option('libdir') / 'atrium' / 'qml' / 'Atrium'

subdir('Shell')

shared_module('atriumplugin',
  'plugin.cpp', 'search.cpp', 'rank.cpp', 'placeholders.cpp', 'launcher_ranking.cpp', 'launcher_catalog.cpp', 'launcher_model.cpp', 'file_search.cpp', 'file_search_core.cpp', 'calc.cpp', 'calc_rates.cpp', 'notes.cpp', 'notes_core.cpp', 'window_layouts.cpp', 'update_sources.cpp', 'compositor.cpp', 'views.cpp', 'notifications.cpp', 'notification_server.cpp', 'clipboard.cpp', 'phone_clipboard.cpp', 'phone_link.cpp', 'brightness.cpp', 'recorder.cpp', 'apps.cpp', 'desktop_files.cpp', 'folder_model.cpp', 'files.cpp', 'levels.cpp', 'session.cpp', 'greeter.cpp', 'unlock.cpp', 'fingerprint.cpp', 'fingerprint_core.cpp', 'mpris.cpp', 'tray.cpp', 'dbus_menu.cpp', 'app_menu.cpp', 'audio.cpp', 'bluetooth.cpp', 'network.cpp', 'keyed_model.cpp', 'sysinfo.cpp', 'sysinfo_core.cpp', 'identify_core.cpp', 'netspeed.cpp', 'netspeed_core.cpp', 'admin_cache.cpp', 'polkit_agent.cpp', 'settings_pages.cpp', 'accounts.cpp', 'requirements.cpp', 'wallpaper.cpp', 'wallpaper_core.cpp', 'default_apps.cpp', 'mimeapps_core.cpp', 'ini_core.cpp', 'keyboard_layouts.cpp', 'keyboard_core.cpp', 'welcome.cpp', 'share.cpp', 'file_chooser.cpp', 'app_chooser.cpp', 'print_prompt.cpp', 'access.cpp', 'capture.cpp', 'capture_core.cpp', 'markup.cpp', 'updates.cpp', 'updates_core.cpp', 'share_core.cpp', 'datetime.cpp', 'region.cpp', 'autostart.cpp', 'battery.cpp', 'disks.cpp', 'themes.cpp', 'printers.cpp', 'printers_core.cpp', 'battery_core.cpp', '../../../src/keyboard_conf.cpp', 'passwd_core.cpp', 'emoji_core.cpp', 'emojis.cpp', '../../../src/accent.cpp', '../../../src/palette.cpp', '../../../src/records.cpp', plugin_moc, version_h, paths_h,
  dependencies: [qt, atrium_runtime_dep, json, dependency('polkit-qt6-agent-1'), dependency('libpulse'), dependency('libpulse-mainloop-glib'), dependency('libxcrypt'), dependency('fontconfig'), dependency('pam')],
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
