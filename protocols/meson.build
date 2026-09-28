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
  wl_protocol_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
  wl_protocol_dir / 'staging/ext-data-control/ext-data-control-v1.xml',
  'wlr-data-control-unstable-v1.xml',
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  'server-decoration.xml',
  wl_protocol_dir / 'staging/xdg-dialog/xdg-dialog-v1.xml',
  wl_protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
  wl_protocol_dir / 'staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml',
  'wlr-layer-shell-unstable-v1.xml',
  wl_protocol_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  wl_protocol_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
  wl_protocol_dir / 'stable/viewporter/viewporter.xml',
  wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
  wl_protocol_dir / 'staging/alpha-modifier/alpha-modifier-v1.xml',
  wl_protocol_dir / 'staging/content-type/content-type-v1.xml',
  wl_protocol_dir / 'staging/tearing-control/tearing-control-v1.xml',
  wl_protocol_dir / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
  wl_protocol_dir / 'stable/presentation-time/presentation-time.xml',
  wl_protocol_dir / 'staging/fifo/fifo-v1.xml',
  wl_protocol_dir / 'staging/commit-timing/commit-timing-v1.xml',
  wl_protocol_dir / 'staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml',
  'drm.xml',
  wl_protocol_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
  wl_protocol_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  wl_protocol_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml',
  wl_protocol_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml',
  wl_protocol_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wl_protocol_dir / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
  wl_protocol_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  wl_protocol_dir / 'staging/pointer-warp/pointer-warp-v1.xml',
  wl_protocol_dir / 'stable/tablet/tablet-v2.xml',
  wl_protocol_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  'wlr-foreign-toplevel-management-unstable-v1.xml',
  wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
  'wlr-output-management-unstable-v1.xml',
  'wlr-output-power-management-unstable-v1.xml',
  'wlr-gamma-control-unstable-v1.xml',
  wl_protocol_dir / 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml',
  'hyprland-global-shortcuts-v1.xml',
  wl_protocol_dir / 'staging/security-context/security-context-v1.xml',
  wl_protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml',
  'input-method-unstable-v2.xml',
  'virtual-keyboard-unstable-v1.xml',
  'wlr-virtual-pointer-unstable-v1.xml',
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
