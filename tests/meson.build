gtest = dependency('gtest', main: true, required: get_option('tests'))

if gtest.found()
  # The scanner's own test protocol: server bindings, and a real client.
  test_xml = files('protocols/atrium-test-v1.xml')
  test_protocol = [
    custom_target('atrium_test_wl',
      input: test_xml,
      output: ['@BASENAME@-server.hpp', '@BASENAME@-server.cpp'],
      command: [wlscanner, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
    ),
    custom_target('atrium_test_client_h',
      input: test_xml,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    ),
    custom_target('atrium_test_code',
      input: test_xml,
      output: '@BASENAME@-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    ),
  ]
  # Client sides of what the protocol layer serves.
  foreach xml : [
    wl_protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
    wl_protocol_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
    wl_protocol_dir / 'staging/ext-data-control/ext-data-control-v1.xml',
    wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  ]
    test_protocol += custom_target(xml.underscorify() + '_client_h',
      input: xml,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
  endforeach

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
    'battery_test.cpp',
    'printers_test.cpp',
    'share_test.cpp',
    'capture_test.cpp',
    'night_light_test.cpp',
    'updates_test.cpp',
    'edid_test.cpp',
    'clipsync_test.cpp',
    'shake_test.cpp',
    'icc_test.cpp',
    'stacking_test.cpp',
    'warp_test.cpp',
    'wlscanner_test.cpp',
    'wl_core_test.cpp',
    'wl_seat_test.cpp',
    'wl_xdg_test.cpp',
    test_protocol,
    files('../shell/plugin/Atrium/search.cpp', '../shell/plugin/Atrium/rank.cpp', '../shell/plugin/Atrium/placeholders.cpp', '../shell/plugin/Atrium/file_search_core.cpp', '../shell/plugin/Atrium/calc.cpp', '../shell/plugin/Atrium/notes_core.cpp', '../shell/plugin/Atrium/files.cpp', '../shell/plugin/Atrium/levels.cpp', '../shell/plugin/Atrium/sysinfo_core.cpp', '../shell/plugin/Atrium/netspeed_core.cpp', '../shell/plugin/Atrium/passwd_core.cpp', '../shell/plugin/Atrium/emoji_core.cpp', '../shell/plugin/Atrium/Shell/desktop_entry_core.cpp', '../shell/plugin/Atrium/wallpaper_core.cpp', '../shell/plugin/Atrium/mimeapps_core.cpp', '../shell/plugin/Atrium/ini_core.cpp', '../shell/plugin/Atrium/battery_core.cpp', '../shell/plugin/Atrium/printers_core.cpp', '../shell/plugin/Atrium/keyboard_core.cpp', '../shell/plugin/Atrium/share_core.cpp', '../shell/plugin/Atrium/capture_core.cpp', '../shell/plugin/Atrium/updates_core.cpp', '../shell/clipsync/clipsync_core.cpp'),
    include_directories: include_directories('..', '../src', '../shell/plugin/Atrium', '../shell/plugin/Atrium/Shell', '../shell/clipsync'),
    dependencies: [atrium_dep, gtest, dependency('libxcrypt'), dependency('wayland-client')],
  )
  test('unit', unit)
endif


# The box tests (tests/box: a whole session in a vc box, and the probe apps
# they drive) are local-only, not part of the repository.
if import('fs').is_dir('box')
  subdir('box')
endif
