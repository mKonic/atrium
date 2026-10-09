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

# Protocols atrium implements itself (wlroots has no helper for them): their
# interface tables too.
own_protocol_xmls = [
  wl_protocol_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml',
  wl_protocol_dir / 'staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml',
  wl_protocol_dir / 'staging/xdg-session-management/xdg-session-management-v1.xml',
  wl_protocol_dir / 'staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml',
  'atrium-glass-v1.xml',
  'appmenu.xml',
]
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
foreach xml : own_protocol_xmls
  protocol_headers += custom_target(
    xml.underscorify() + '_server_h',
    input: xml,
    output: '@BASENAME@-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_code += custom_target(
    xml.underscorify() + '_code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# For atrium's own Wayland clients (atrium-portal's screen sharing,
# atrium-screenshot, atrium-record): capture, windows and buffers.
client_protocol_xmls = [
  wl_protocol_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  wl_protocol_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  wl_protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
  wl_protocol_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
]
capture_client_protocols = []
foreach xml : client_protocol_xmls
  capture_client_protocols += custom_target(xml.underscorify() + '_client_h', input: xml, output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  capture_client_protocols += custom_target(xml.underscorify() + '_client_code', input: xml, output: '@BASENAME@-client-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach
