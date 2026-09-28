wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable(pkgconfig: 'wayland_scanner'),
  native: true,
)
wl_protocol_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

protocol_xmls = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  wl_protocol_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml',
  'wlr-layer-shell-unstable-v1.xml',
  'wlr-output-power-management-unstable-v1.xml',
  'wlr-foreign-toplevel-management-unstable-v1.xml',
]

protocol_headers = []
foreach xml : protocol_xmls
  protocol_headers += custom_target(
    xml.underscorify() + '_server_h',
    input: xml,
    output: '@BASENAME@-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

protocol_code = []
# Interfaces the own protocols refer to (xdg-toplevel-drag names
# xdg_toplevel), which wlroots keeps to itself.
foreach xml : [wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml']
  protocol_code += custom_target(
    xml.underscorify() + '_code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# Protocols on atrium's own bindings (tools/wlscanner.py: a C++ class per
# interface over libwayland), with wayland-scanner's interface tables.
wlscanner = find_program('../tools/wlscanner.py')
wl_protocol_xmls = [
  wl_protocol_dir / 'staging/xdg-system-bell/xdg-system-bell-v1.xml',
  wl_protocol_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml',
  'atrium-glass-v1.xml',
  wl_protocol_dir / 'staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml',
  wl_protocol_dir / 'staging/xdg-session-management/xdg-session-management-v1.xml',
  wl_protocol_dir / 'staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml',
  wl_protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
]
wl_protocol_sources = []
wl_protocol_headers = []
# The core protocol: its interface tables are libwayland-server's own.
wl_core = custom_target(
  'wayland_wl',
  input: wayland_dir / 'wayland.xml',
  output: ['wayland-server.hpp', 'wayland-server.cpp'],
  command: [wlscanner, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
)
wl_protocol_sources += wl_core
wl_protocol_headers += wl_core[0]
foreach xml : wl_protocol_xmls
  wl = custom_target(
    xml.underscorify() + '_wl',
    input: xml,
    output: ['@BASENAME@-server.hpp', '@BASENAME@-server.cpp'],
    command: [wlscanner, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
  )
  wl_protocol_sources += wl
  wl_protocol_headers += wl[0]
  protocol_code += custom_target(
    xml.underscorify() + '_code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach
