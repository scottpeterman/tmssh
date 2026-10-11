# gtkpyte/meson.build -- the vendored terminal widget and its emulation core.
# See VENDORED.md for where these files come from. Nothing under gtkpyte/
# is edited here; changes go to gtkpyte first and are copied back.

utf8proc_dep = subproject('utf8proc').get_variable('utf8proc_dep')

pyte_inc = include_directories('pyte/include')

pyte_lib = static_library('pyte_core',
  'pyte/src/rowstore.cpp',
  'pyte/src/screen.cpp',
  'pyte/src/stream.cpp',
  'pyte/src/unicode.cpp',
  'pyte/src/screen_range.cpp',
  include_directories : pyte_inc,
  dependencies : utf8proc_dep,
)

pyte_dep = declare_dependency(
  link_with : pyte_lib,
  include_directories : pyte_inc,
)

util_dep = meson.get_compiler('cpp').find_library('util', required : false)  # forkpty

gtkpyte_lib = static_library('gtkpyte',
  'src/gtkpyte-terminal.cpp',
  'src/keymap.cpp',
  'src/mouse.cpp',
  'src/palette.cpp',
  'src/pty.cpp',
  'src/ptysession.cpp',
  'src/schemes.cpp',
  'src/selection.cpp',
  dependencies : [pyte_dep, gtk_dep, util_dep],
)

gtkpyte_dep = declare_dependency(
  link_with : gtkpyte_lib,
  include_directories : include_directories('src'),
  dependencies : [pyte_dep, gtk_dep, util_dep],
)
