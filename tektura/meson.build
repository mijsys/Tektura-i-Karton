project('tektura', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c17'],
)

# Autor: MijagiKutasamoto

wlroots = dependency('wlroots-0.18')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
openssl = dependency('openssl')  # HMAC-SHA256 dla integralności bazy uprawnień

tektura_sources = [
  'src/main.c',
  'src/server.c',
  'src/output.c',
  'src/input.c',
  'src/view.c',
  'src/protocols.c',
  'src/security_manager.c',
  'src/ipc.c',
  'src/i18n.c',
  'src/lock.c',
  'src/workspace.c',
  'src/config.c',
  'src/idle.c',
  'src/layer.c',
  'src/decoration.c',
  'src/xwayland.c',
  'src/wallpaper.c',
]

executable('tektura',
  tektura_sources,
  dependencies: [wlroots, wayland_server, xkbcommon, openssl],
  install: true,
)
