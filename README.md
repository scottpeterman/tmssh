# tmssh

Tiny Managed SSH.

A small GTK 4 session-manager terminal for Linux: a session tree, tabs, and
ssh or telnet sessions on the [gtkpyte](https://github.com/scottpeterman/gtkpyte)
widget. No vault, no serial.

Status: the session model only, with no UI yet.

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

```
sudo apt install -y build-essential git pkg-config meson ninja-build libyaml-cpp-dev libgtk-4-dev
```

```
scripts/build.sh
```

`scripts/build.sh` configures, builds and runs the tests. The first run
fetches doctest at a pinned tag. `--release`, `--clean`, `--no-tests` and
`--deps` work as in gtkpyte.

## Layout

```
src/sessions.{h,cpp}     session model, YAML load/save, tree edits, import
tests/test_sessions.cpp  doctest suite, incl. a file written by PathfinderSSH
scripts/build.sh
```

## Licence

GPL-3.0; see `LICENSE`.
