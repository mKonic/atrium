gtest = dependency('gtest', main: true, required: get_option('tests'))

if gtest.found()
  unit = executable('unit',
    'geometry_test.cpp',
    'config_test.cpp',
    'settings_test.cpp',
    'rules_test.cpp',
    'search_test.cpp',
    'files_test.cpp',
    'registry_test.cpp',
    'sandbox_test.cpp',
    'accent_test.cpp',
    files('../src/geometry.cpp', '../src/config.cpp', '../src/settings.cpp', '../src/rules.cpp', '../src/registry.cpp', '../src/sandbox.cpp', '../src/accent.cpp', '../src/palette.cpp', '../src/devices.cpp', '../shell/plugin/Atrium/search.cpp', '../shell/plugin/Atrium/files.cpp', '../shell/plugin/Atrium/levels.cpp', '../shell/plugin/Atrium/sysinfo_core.cpp', '../shell/plugin/Atrium/netspeed_core.cpp', '../shell/plugin/Atrium/passwd_core.cpp'),
    protocol_headers, paths_h,
    include_directories: include_directories('..', '../src', '../shell/plugin/Atrium'),
    dependencies: deps + [gtest, dependency('libxcrypt')],
  )
  test('unit', unit)
endif


# A throwaway input method the smoke test types with (tests/smoke.sh).
if add_languages('c', required: false, native: false)
  ime_xml = meson.project_source_root() / 'protocols/input-method-unstable-v2.xml'
  ime_client_h = custom_target('ime_client_h',
    input: ime_xml, output: 'input-method-unstable-v2-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  ime_code = custom_target('ime_code',
    input: ime_xml, output: 'input-method-unstable-v2-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  executable('ime_probe', 'ime_probe.c', ime_client_h, ime_code,
    dependencies: dependency('wayland-client'),
    c_args: ['-Wno-unused-parameter'])

  # An X11 window of any _NET_WM_WINDOW_TYPE.
  xcb_dep = dependency('xcb', required: false)
  if xcb_dep.found()
    executable('x11_probe', 'x11_probe.c', dependencies: xcb_dep)
  endif
endif
