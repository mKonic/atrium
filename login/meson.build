# atrium-login, the display manager (a root daemon), and its system files.
login_core = files('core.cpp', '../shell/plugin/Atrium/ini_core.cpp')
login_inc = include_directories('.', '../shell/plugin/Atrium')

login_deps = [json, dependency('pam')]
if libsystemd.found()
  login_deps += libsystemd
endif

executable('atrium-login',
  'main.cpp', 'daemon.cpp', 'worker.cpp', login_core,
  include_directories: login_inc,
  dependencies: login_deps,
  install: true,
  install_dir: get_option('prefix') / get_option('sbindir'),
)

login_unit = configuration_data()
login_unit.set('SBINDIR', get_option('prefix') / get_option('sbindir'))
configure_file(input: 'data/atrium-login.service.in', output: 'atrium-login.service', configuration: login_unit,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'system')
install_data('data/sysusers.conf', rename: 'atrium-login.conf',
  install_dir: get_option('prefix') / 'lib' / 'sysusers.d')
install_data('data/tmpfiles.conf', rename: 'atrium-login.conf',
  install_dir: get_option('prefix') / 'lib' / 'tmpfiles.d')
install_data('data/pam/atrium-login', 'data/pam/atrium-greeter', 'data/pam/atrium-autologin', 'data/pam/atrium-lock',
  install_dir: get_option('sysconfdir') / 'pam.d')
install_data('data/login.conf', install_dir: get_option('datadir') / 'atrium')
