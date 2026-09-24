# atrium-shell: runs a shell written in QML (the desktop, System Settings,
# the login screen) with the Atrium and Atrium.Shell modules.
host_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'Gui', 'Widgets', 'Qml', 'Quick'])

executable('atrium-shell', 'main.cpp', paths_h,
  dependencies: host_qt,
  include_directories: include_directories('.', '../..'),
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install: true,
)
