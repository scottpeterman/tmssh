project('tmssh', ['c', 'cpp'],
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  meson_version : '>= 1.0.0',
  default_options : [
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

gnome = import('gnome')

# Debian 12 ships yaml-cpp 0.7.0 and GTK 4.8; nothing here needs newer.
yaml_dep = dependency('yaml-cpp', version : '>= 0.7.0')
gtk_dep = dependency('gtk4', version : '>= 4.8')

# The terminal widget, vendored from gtkpyte. Provides gtkpyte_dep.
subdir('gtkpyte')

# Session model and theme data: GTK-free, so the tests need no display.
core_lib = static_library('tmssh_core',
  'src/sessions.cpp',
  'src/session_log.cpp',
  'src/settings.cpp',
  'src/ssh_command.cpp',
  'src/themes.cpp',
  dependencies : [yaml_dep],
)

core_dep = declare_dependency(
  link_with : core_lib,
  include_directories : include_directories('src'),
  dependencies : [yaml_dep],
)

# The bundled theme pack. Compiled straight into the executable rather than a
# static library: the resource registers itself from a constructor, and a
# linker is free to drop an unreferenced object out of an archive.
theme_resources = gnome.compile_resources('tmssh-resources',
  'data/tmssh.gresource.xml',
  source_dir : 'data',
)

# Our own sources are held to the GTK 4.8 API (Debian 12, Crostini): using
# anything newer is a compile-time warning, not a surprise on the Chromebook.
gtk_floor_args = [
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_8',
  '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_8',
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_74',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_74',
]

executable('tmssh',
  'src/main.cpp',
  'src/app_window.cpp',
  'src/theme_apply.cpp',
  theme_resources,
  dependencies : [core_dep, gtkpyte_dep],
  cpp_args : gtk_floor_args,
  install : true,
)

if get_option('tests')
  doctest_dep = subproject('doctest').get_variable('doctest_dep')
  tmssh_tests = executable('tmssh_tests',
    'tests/main.cpp',
    'tests/test_session_log.cpp',
    'tests/test_sessions.cpp',
    'tests/test_settings.cpp',
    'tests/test_ssh_command.cpp',
    'tests/test_themes.cpp',
    dependencies : [core_dep, doctest_dep],
    cpp_args : ['-DTMSSH_THEMES_DIR="' + meson.project_source_root() / 'data' / 'themes' + '"'],
  )
  test('tmssh_tests', tmssh_tests)
endif

# Exec gets the absolute path: launchers (the ChromeOS shelf included) do not
# reliably start apps with ~/.local/bin on PATH.
desktop_conf = configuration_data()
desktop_conf.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(
  input : 'data/io.github.scottpeterman.tmssh.desktop.in',
  output : 'io.github.scottpeterman.tmssh.desktop',
  configuration : desktop_conf,
  install : true,
  install_dir : get_option('datadir') / 'applications',
)

install_data('data/io.github.scottpeterman.tmssh.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_data('LICENSE', 'THIRD-PARTY-NOTICES.md', 'README.md',
  install_dir : get_option('datadir') / 'doc' / meson.project_name(),
)
install_subdir('licenses',
  install_dir : get_option('datadir') / 'doc' / meson.project_name(),
)
