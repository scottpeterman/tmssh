project('tmssh', ['cpp'],
  version : '0.0.1',
  license : 'GPL-3.0-or-later',
  meson_version : '>= 1.0.0',
  default_options : [
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

# Debian 12 ships 0.7.0, Ubuntu 24.04 0.8.0; nothing here needs newer.
yaml_dep = dependency('yaml-cpp', version : '>= 0.7.0')

# The session model: GTK-free, so the tests need no display.
sessions_lib = static_library('tmssh_sessions',
  'src/sessions.cpp',
  dependencies : [yaml_dep],
)

sessions_dep = declare_dependency(
  link_with : sessions_lib,
  include_directories : include_directories('src'),
)

if get_option('tests')
  doctest_dep = subproject('doctest').get_variable('doctest_dep')
  tmssh_tests = executable('tmssh_tests',
    'tests/main.cpp',
    'tests/test_sessions.cpp',
    dependencies : [sessions_dep, doctest_dep],
  )
  test('tmssh_tests', tmssh_tests)
endif
