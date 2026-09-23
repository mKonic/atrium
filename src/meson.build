atrium_sources = files(
  'anim.cpp',
  'config.cpp',
  'geometry.cpp',
  'ipc.cpp',
  'layer_surface.cpp',
  'main.cpp',
  'output.cpp',
  'seat.cpp',
  'server.cpp',
  'rules.cpp',
  'session_lock.cpp',
  'space.cpp',
  'spaces.cpp',
  'settings.cpp',
  'theme.cpp',
  'titlebar.cpp',
  'view.cpp',
  'xdg_view.cpp',
  'xwayland_view.cpp',
)

executable('atrium',
  atrium_sources, protocol_headers, version_h,
  dependencies: deps,
  install: true,
)

executable('atriumctl',
  '../tools/atriumctl.cpp',
  dependencies: [json],
  install: true,
)
