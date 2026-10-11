# tmssh

Tiny Managed SSH.

A small GTK 4 session-manager terminal for Linux: a session tree, tabs, and
ssh or telnet sessions on the [gtkpyte](https://github.com/scottpeterman/gtkpyte)
widget. No vault, no serial.

Status: session model and themes. The app currently opens one terminal on a
local program; the session tree and tabs come next.

## Running

```
./build/tmssh
./build/tmssh --theme gruvbox-dark --app light
./build/tmssh --font "JetBrains Mono 12"
./build/tmssh --list-themes
./build/tmssh -- htop
```

`--app` sets the window chrome, dark or light. `--theme` sets the terminal
palette. The two are independent; the default is a dark window around the
light `ice` terminal.

## Themes

30 terminal themes are compiled in, from PathfinderSSH's theme pack. To add
one, or replace a bundled one, drop a YAML file in `~/.config/tmssh/themes/`
(or `$XDG_CONFIG_HOME/tmssh/themes/`). A file whose `name` matches a bundled
theme replaces it. PathfinderSSH theme files work unchanged:

```yaml
name: "gruvbox-dark"
label: "Gruvbox Dark"
type: "dark"
terminal:
  background: "#282828"
  foreground: "#ebdbb2"
  cursor: "#ebdbb2"
  selection: "#504945"
  black: "#282828"
  red: "#cc241d"
  # green, yellow, blue, magenta, cyan, white,
  # then bright_black through bright_white
```

All 16 ANSI colours, `background` and `foreground` are required. Colours are
`#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb(r, g, b)` or `rgba(r, g, b, a)`. The
selection is drawn over the text, so an opaque selection colour is given
transparency.

## Session file

`~/.config/tmssh/sessions.yaml` (or `$XDG_CONFIG_HOME/tmssh/sessions.yaml`).
The format is PathfinderSSH's `sessions.yaml`, so an existing
`~/.pathfinderssh/sessions.yaml` imports directly:

```yaml
version: 1
folders:
  - folder_name: Lab
    sessions:
      - name: eng-spine-1
        transport: ssh
        host: 172.16.2.5
        username: admin
        terminal_theme: ice
```

Folders are one level deep. Serial sessions are skipped on load. Keys tmssh
does not model (`credential`, `anti_idle`, paste pacing, `vendor`, `notes`, ...)
are ignored and not written back.

## Building

Debian 12 or later, Ubuntu 22.04 or later, or ChromeOS Linux (Crostini).
GTK 4.8, yaml-cpp 0.7 and Meson 1.0 are the floor.

```
sudo apt install -y build-essential git pkg-config meson ninja-build libyaml-cpp-dev libgtk-4-dev
```

```
scripts/build.sh
```

`scripts/build.sh` configures, builds and runs the tests. The first run
needs network access: Meson fetches utf8proc and doctest at pinned tags.

| Command | Result |
|---|---|
| `scripts/build.sh --release` | release build in `build-release/` |
| `scripts/build.sh --clean` | wipe the build directory first |
| `scripts/build.sh --no-tests` | skip the tests (doctest is not fetched) |
| `scripts/build.sh --deps` | apt-install the toolchain first |
| `scripts/build.sh --install` | install to `~/.local`, with a launcher entry |
| `scripts/build.sh --run -- --theme nord` | build, then run tmssh with these options |

`scripts/build.sh --release --install` puts tmssh in the desktop's
application menu; on a Chromebook it appears under Linux apps.

## Layout

```
src/
  sessions.{h,cpp}       session model, YAML load/save, tree edits, import
  themes.{h,cpp}         theme files, colour parsing, the theme set
  theme_apply.{h,cpp}    themes onto the widget, app dark/light, the bundled pack
  main.cpp               the app
gtkpyte/                 the terminal widget, vendored (see gtkpyte/VENDORED.md)
data/themes/             the bundled theme pack, compiled in as a GResource
tests/                   doctest suites for sessions and themes
scripts/build.sh
```

`sessions` and `themes` have no GTK dependency and are tested directly.

## Licence

GPL-3.0; see `LICENSE`. Third-party components and their licences are listed
in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), with full texts in
`licenses/`.
