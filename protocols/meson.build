wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable(pkgconfig: 'wayland_scanner'),
  native: true,
)
wl_protocol_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

protocol_xmls = [
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
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
