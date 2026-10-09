gtest = dependency('gtest', main: true, required: get_option('tests'))

if gtest.found()
  unit = executable('unit',
    'geometry_test.cpp',
    'config_test.cpp',
    'settings_test.cpp',
    'rules_test.cpp',
    'search_test.cpp',
    'rank_test.cpp',
    'placeholders_test.cpp',
    'file_search_test.cpp',
    'calc_test.cpp',
    'keyword_watch_test.cpp',
    'notes_test.cpp',
    'files_test.cpp',
    'registry_test.cpp',
    'sandbox_test.cpp',
    'accent_test.cpp',
    'desktop_entry_test.cpp',
    'wallpaper_test.cpp',
    'mimeapps_test.cpp',
    'keyboard_test.cpp',
    'ini_test.cpp',
    'wrapper_test.cpp',
    'fingerprint_test.cpp',
    'idle_test.cpp',
    'clipboard_test.cpp',
    'logout_test.cpp',
    'login_test.cpp',
    'battery_test.cpp',
    'printers_test.cpp',
    'share_test.cpp',
    'capture_test.cpp',
    'night_light_test.cpp',
    'updates_test.cpp',
    'edid_test.cpp',
    'clipsync_test.cpp',
    'phonelink_test.cpp',
    'keyring_test.cpp',
    'portal_test.cpp',
    'shake_test.cpp',
    'snapshot_test.cpp',
    files('../src/geometry.cpp', '../src/snapshot_core.cpp', '../src/clipboard_core.cpp', '../src/shake.cpp', '../login/core.cpp', '../src/logout_core.cpp', '../src/idle_core.cpp', '../src/edid.cpp', '../src/night_light_core.cpp', '../src/keyboard_conf.cpp', '../src/config.cpp', '../src/settings.cpp', '../src/rules.cpp', '../src/registry.cpp', '../src/records.cpp', '../src/keyword_watch.cpp', '../src/sandbox.cpp', '../src/accent.cpp', '../src/palette.cpp', '../src/devices.cpp', '../shell/plugin/Atrium/search.cpp', '../shell/plugin/Atrium/fingerprint_core.cpp', '../shell/plugin/Atrium/rank.cpp', '../shell/plugin/Atrium/placeholders.cpp', '../shell/plugin/Atrium/file_search_core.cpp', '../shell/plugin/Atrium/calc.cpp', '../shell/plugin/Atrium/notes_core.cpp', '../shell/plugin/Atrium/files.cpp', '../shell/plugin/Atrium/levels.cpp', '../shell/plugin/Atrium/sysinfo_core.cpp', '../shell/plugin/Atrium/netspeed_core.cpp', '../shell/plugin/Atrium/passwd_core.cpp', '../shell/plugin/Atrium/emoji_core.cpp', '../shell/plugin/Atrium/Shell/desktop_entry_core.cpp', '../shell/plugin/Atrium/wallpaper_core.cpp', '../shell/plugin/Atrium/mimeapps_core.cpp', '../shell/plugin/Atrium/ini_core.cpp', '../shell/plugin/Atrium/battery_core.cpp', '../shell/plugin/Atrium/printers_core.cpp', '../shell/plugin/Atrium/keyboard_core.cpp', '../shell/plugin/Atrium/share_core.cpp', '../shell/plugin/Atrium/capture_core.cpp', '../shell/plugin/Atrium/updates_core.cpp', '../shell/clipsync/clipsync_core.cpp', '../shell/phonelink/crypto.cpp', '../shell/phonelink/link_core.cpp', '../shell/phonelink/jitter.cpp', '../keyring/keyring_core.cpp', '../shell/plugin/Atrium/portal_core.cpp', '../src/wrapper.cpp'),
    protocol_headers, paths_h,
    include_directories: include_directories('..', '../src', '../shell/plugin/Atrium', '../shell/plugin/Atrium/Shell', '../shell/clipsync', '../shell/phonelink', '../keyring'),
    dependencies: deps + [gtest, dependency('libcrypto'), dependency('libxcrypt')],
  )
  test('unit', unit)

  # The shell plugin's Qt classes, on QtCore and QtDBus (no QML, no display).
  unit_qt_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'DBus'])
  unit_qt = executable('unit-qt',
    'folder_model_test.cpp', 'file_chooser_test.cpp', 'app_chooser_test.cpp', 'print_prompt_test.cpp', 'dbus_menu_test.cpp',
    files('../shell/plugin/Atrium/folder_model.cpp', '../shell/plugin/Atrium/files.cpp', '../shell/plugin/Atrium/file_chooser.cpp', '../shell/plugin/Atrium/app_chooser.cpp', '../shell/plugin/Atrium/Shell/desktop_entry_core.cpp', '../shell/plugin/Atrium/print_prompt.cpp', '../shell/plugin/Atrium/printers_core.cpp', '../shell/plugin/Atrium/dbus_menu.cpp'),
    qt6.compile_moc(headers: ['../shell/plugin/Atrium/folder_model.hpp', '../shell/plugin/Atrium/file_chooser.hpp', '../shell/plugin/Atrium/app_chooser.hpp', '../shell/plugin/Atrium/print_prompt.hpp'], dependencies: unit_qt_qt),
    include_directories: include_directories('../shell/plugin/Atrium', '../shell/plugin/Atrium/Shell'),
    dependencies: [gtest, unit_qt_qt],
  )
  test('unit-qt', unit_qt)
endif


# The box tests (tests/box: a whole session in a vc box, and the probe apps
# they drive) are local-only, not part of the repository.
if import('fs').is_dir('box')
  subdir('box')
endif
