project('karton-shell', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c17'],
)

gtk = dependency('gtk4')
adw = dependency('libadwaita-1')
openssl = dependency('openssl')

shell_sources = [
  'src/main.c',
]

executable('karton-shell',
  shell_sources,
  dependencies: [gtk, adw, openssl],
  c_args: ['-DKARTON_THEME_DIR="' + meson.current_source_dir() + '/../../theme"'],
  install: false,
)
