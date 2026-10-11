# Third-party notices

tmssh is licensed under the GNU General Public License v3.0; see `LICENSE`.
It incorporates or links the components below. Full licence texts are in
`licenses/`, and `meson install` places this file and that directory under
`share/doc/tmssh/`.

Maintained by hand: update this file when a dependency is added, removed
or re-pinned.

## Compiled into tmssh

### gtkpyte (`gtkpyte/src/`)

- Copyright Scott Peterman
- GNU General Public License v3.0
- Vendored from https://github.com/scottpeterman/gtkpyte; the commit is in
  `gtkpyte/VENDORED.md`

The GTK 4 terminal widget and the local-pty session.

### pyte C++ core (`gtkpyte/pyte/`) -- not the Python pyte library

- Copyright Scott Peterman
- GNU General Public License v3.0
- Vendored via gtkpyte from https://github.com/scottpeterman/anytermqt

The headless terminal emulator under the widget: escape-sequence parser,
screen buffer, scrollback. It descends from Python pyte by way of the Go
port gopyte (https://github.com/scottpeterman/pathfinderssh/tree/main/internal/gopyte)
and is a separate implementation.

### pyte (Python), the original

- Copyright George Shuklin, Sergei Lebedev and contributors
- GNU Lesser General Public License v3.0 (`licenses/LGPL-3.0.txt`)
- https://github.com/selectel/pyte

The design the ports follow. The ports are translations rather than copies,
and as derived works they are conveyed under GPL-3.0, which LGPL-3.0
permits.

### utf8proc

- Copyright 2014-2021 Steven G. Johnson, Jiahao Chen, Tony Kelman,
  Jonas Fonseca and other contributors
- Copyright 2009, 2013 Public Software Group e. V., Berlin, Germany
- MIT "expat" licence
- Unicode data tables: Copyright 1991-2007 Unicode, Inc., distributed under
  the Unicode Terms of Use
- Version 2.11.3, fetched by `subprojects/utf8proc.wrap` and statically
  linked
- https://github.com/JuliaStrings/utf8proc

Full text, including the Unicode data licence: `licenses/utf8proc.md`.

### Theme pack (`data/themes/`)

- Copyright Scott Peterman
- GNU General Public License v3.0
- From https://github.com/scottpeterman/pathfinderssh (`internal/ui/themes/`)

Compiled into the binary as a GResource. Several themes reproduce published
palettes, credited to their designers:

| Themes | Designer | Licence of the original |
|---|---|---|
| solarized-dark, solarized-light, solarized-hybrid | Ethan Schoonover, https://ethanschoonover.com/solarized/ | MIT, Copyright 2011 Ethan Schoonover |
| gruvbox-dark, gruvbox-light, gruvbox-hybrid | morhetz, https://github.com/morhetz/gruvbox | MIT/X11 |
| nord | Sven Greb, https://www.nordtheme.com/ | MIT, Copyright 2016-present Sven Greb |
| catppuccin-latte, catppuccin-mocha | Catppuccin, https://catppuccin.com/ | MIT, Copyright 2021 Catppuccin |

## Linked from the system

Dynamically linked against the unmodified shared libraries the platform
provides. tmssh is conveyed under GPL-3.0, which these licences permit.

### GTK 4, GLib, Pango

- Copyright the GTK, GLib and Pango authors
- GNU Lesser General Public License v2.1 or later (`licenses/LGPL-2.1.txt`)
- https://gtk.org/

### yaml-cpp

- Copyright 2008-2015 Jesse Beder
- MIT licence (`licenses/yaml-cpp.txt`)
- https://github.com/jbeder/yaml-cpp

Reads and writes the session file and the theme files.

### GNU C Library (forkpty, libutil)

- Copyright the Free Software Foundation and contributors
- GNU Lesser General Public License v2.1 or later
- https://www.gnu.org/software/libc/

## Build and test only (not in any installed binary)

### doctest

- Copyright 2016-2023 Viktor Kirilov
- MIT licence (`licenses/doctest.txt`)
- Version 2.5.3, fetched by `subprojects/doctest.wrap`
- https://github.com/doctest/doctest

### Meson, Ninja

Build tools; nothing from either is included in the output.
