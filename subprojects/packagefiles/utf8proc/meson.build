project('utf8proc', 'c', version : '2.11.3', meson_version : '>= 1.0.0')

utf8proc_lib = static_library('utf8proc', 'utf8proc.c',
  c_args : ['-DUTF8PROC_STATIC'],
  pic : true,
)

utf8proc_dep = declare_dependency(
  link_with : utf8proc_lib,
  include_directories : include_directories('.'),
  compile_args : ['-DUTF8PROC_STATIC'],
)
