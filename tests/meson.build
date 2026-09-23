gtest = dependency('gtest', main: true, required: get_option('tests'))

if gtest.found()
  unit = executable('unit',
    'geometry_test.cpp',
    'config_test.cpp',
    'settings_test.cpp',
    files('../src/geometry.cpp', '../src/config.cpp', '../src/settings.cpp'),
    protocol_headers,
    include_directories: include_directories('../src'),
    dependencies: deps + [gtest],
  )
  test('unit', unit)
endif
