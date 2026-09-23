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
    files('../src/geometry.cpp', '../src/config.cpp', '../src/settings.cpp', '../src/rules.cpp', '../src/registry.cpp', '../src/sandbox.cpp', '../shell/plugin/Atrium/search.cpp', '../shell/plugin/Atrium/files.cpp', '../shell/plugin/Atrium/levels.cpp', '../shell/plugin/Atrium/sysinfo_core.cpp', '../shell/plugin/Atrium/netspeed_core.cpp'),
    protocol_headers,
    include_directories: include_directories('../src', '../shell/plugin/Atrium'),
    dependencies: deps + [gtest],
  )
  test('unit', unit)
endif

