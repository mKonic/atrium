# atrium-keyring: the Secret Service (see service.hpp), and pam_atrium_keyring
# that hands it the login password.
keyring_core = files('keyring_core.cpp')
keyring_qt = dependency('qt6', version: '>=6.8', modules: ['Core', 'DBus'])
keyring_moc = qt6.compile_moc(headers: ['service.hpp'], dependencies: keyring_qt)

executable('atrium-keyring', 'main.cpp', 'service.cpp', keyring_core, keyring_moc,
  dependencies: [keyring_qt, json, dependency('libcrypto'), dependency('pam')],
  cpp_args: meson.get_compiler('cpp').get_supported_arguments('-Wno-sfinae-incomplete'),
  override_options: ['b_sanitize=none'],
  install: true,
)

shared_module('pam_atrium_keyring', 'pam_atrium_keyring.c',
  name_prefix: '',
  dependencies: [dependency('pam')],
  install: true,
  install_dir: get_option('prefix') / 'lib' / 'security',
)
