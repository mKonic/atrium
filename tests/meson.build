gtest = dependency('gtest', main: true, required: get_option('tests'))

if gtest.found()
  unit = executable('unit',
    'geometry_test.cpp',
    'config_test.cpp',
    'settings_test.cpp',
    'placements_test.cpp',
    'rules_test.cpp',
    files('../src/geometry.cpp', '../src/config.cpp', '../src/settings.cpp', '../src/rules.cpp', '../src/placements.cpp'),
    protocol_headers,
    include_directories: include_directories('../src'),
    dependencies: deps + [gtest],
  )
  test('unit', unit)
endif
