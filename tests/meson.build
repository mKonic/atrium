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
    files('../src/geometry.cpp', '../src/config.cpp', '../src/settings.cpp', '../src/rules.cpp', '../src/registry.cpp', '../src/sandbox.cpp', '../src/accent.cpp', '../src/palette.cpp', '../src/devices.cpp', '../shell/plugin/Atrium/search.cpp', '../shell/plugin/Atrium/files.cpp', '../shell/plugin/Atrium/levels.cpp', '../shell/plugin/Atrium/sysinfo_core.cpp', '../shell/plugin/Atrium/netspeed_core.cpp', '../shell/plugin/Atrium/passwd_core.cpp', '../shell/plugin/Atrium/emoji_core.cpp'),
    protocol_headers, paths_h,
    include_directories: include_directories('..', '../src', '../shell/plugin/Atrium'),
    dependencies: deps + [gtest, dependency('libxcrypt')],
  )
  test('unit', unit)
endif


# The box tests (tests/box: a whole session in a vc box, and the probe apps
# they drive) are local-only, not part of the repository.
if import('fs').is_dir('box')
  subdir('box')
endif
