# tmssh

Tiny Managed SSH.

A small GTK 4 session-manager terminal for Linux: a session tree, tabs, and
ssh or telnet sessions on the [gtkpyte](https://github.com/scottpeterman/gtkpyte)
widget. No vault, no serial.

Status: session tree, tabs, ssh, themes. Telnet comes next; until then a
telnet session opens a placeholder tab.

## Running

```
tmssh
tmssh --theme gruvbox-dark --app light
tmssh --font "JetBrains Mono 12"
tmssh --sessions ~/lab/sessions.yaml
tmssh --list-themes
```

`--theme`, `--app` and `--font` apply to that run only. Choices made from the
menu are saved to `~/.config/tmssh/settings.yaml`.

Double-click a session (or select it and press Enter) to open it in a new
tab. Right-click a session or folder to connect or delete it. Type in the
filter box to narrow the tree by name, host or username.

| Keys | Action |
|---|---|
| Ctrl+Shift+T | new local terminal |
| Ctrl+Shift+W | close tab |
| Ctrl+Shift+R | reconnect the current tab |
| Ctrl+PageUp / Ctrl+PageDown | previous / next tab |
| Ctrl+Shift+F | filter sessions |
| Ctrl+Shift+Q | quit |
| Ctrl+Shift+C / Ctrl+Shift+V | copy / paste (in the terminal) |

When an ssh session ends, its tab stays open with the exit status, so an
authentication or connection error stays readable. Ctrl+Shift+R runs it
again.

## Editing sessions

Sessions are edited in the session file itself. **Edit Session File** in the
menu opens it in the desktop's editor for YAML files, or, when there is
none, in `$VISUAL`, `$EDITOR`, `sensible-editor`, `nano` or `vi` in a new tab
(which closes itself when the editor exits cleanly). tmssh reloads the tree
whenever the file is saved, from any editor. A file that will not parse is
reported above the tree, and the last good tree stays until it is fixed.

Deleting from the tree and importing rewrite the file, which drops any
comments in it.

## First run

With no `~/.config/tmssh/sessions.yaml`, tmssh writes one: a `Local` folder
holding an ssh session to `localhost` as you, and a comment block listing
every key a session can carry.

If `~/.pathfinderssh/sessions.yaml` exists, tmssh first offers to import it
instead; **Start Empty** writes the localhost file. Either answer is
remembered, and PathfinderSSH's file is only ever read. The menu has
**Import PathfinderSSH Sessions** and **Import Session File…** for later;
an import adds what is new and leaves existing sessions alone.

## How ssh sessions run

An ssh session is the system `ssh` client in the tab's pty, so
`~/.ssh/config`, the agent, ProxyJump and every prompt (password,
passphrase, host key) work as they do in any terminal. Each tab starts by
printing the exact command it runs. Session fields map onto ssh like this:

| Session field | ssh |
|---|---|
| `port`, `username` | `-p`, `-l` |
| `auth_type: agent` | nothing; ssh's own order |
| `auth_type: password` | `PreferredAuthentications=keyboard-interactive,password`, `PubkeyAuthentication=no` |
| `auth_type: publickey`, `key_path` | `-i key`, `IdentitiesOnly=yes` |
| `host_key_policy` strict / tofu / insecure | `StrictHostKeyChecking` yes / accept-new / no (insecure also skips known_hosts) |
| `known_hosts` | `UserKnownHostsFile` |
| `connect_timeout_sec` | `ConnectTimeout` |
| `jump` | `-J user@host:port`, or a `ProxyCommand` when the jump has its own key |
| `legacy_algorithms` | old KEX, ciphers, MACs and host keys appended with `+`, cut to what the local ssh supports |
| `term_type` | `TERM` |

## Themes

30 terminal themes are compiled in, from PathfinderSSH's theme pack. The
window (dark or light) and the terminal theme are chosen separately from the
menu; a session with its own `terminal_theme` keeps it. To add a theme, or
replace a bundled one, drop a YAML file in `~/.config/tmssh/themes/`. A file
whose `name` matches a bundled theme replaces it. PathfinderSSH theme files
work unchanged:

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

## Files

| File | What |
|---|---|
| `~/.config/tmssh/sessions.yaml` | sessions |
| `~/.config/tmssh/settings.yaml` | window, theme, font, scrollback, window size |
| `~/.config/tmssh/themes/` | your themes |

`$XDG_CONFIG_HOME` replaces `~/.config` when it is set.

The session file is PathfinderSSH's format:

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
GTK 4.8, GLib 2.74, yaml-cpp 0.7 and Meson 1.0 are the floor; tmssh's own
sources are compiled against the GTK 4.8 API, so anything newer is a build
warning.

```
sudo apt install -y build-essential git pkg-config meson ninja-build libyaml-cpp-dev libgtk-4-dev librsvg2-common openssh-client
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
  ssh_command.{h,cpp}    a session as an ssh command line
  settings.{h,cpp}       settings.yaml
  themes.{h,cpp}         theme files, colour parsing, the theme set
  theme_apply.{h,cpp}    themes onto the widget, app dark/light, the bundled pack
  app_window.{h,cpp}     the window: session tree, tabs, menus, dialogs
  main.cpp               options and startup
gtkpyte/                 the terminal widget, vendored (see gtkpyte/VENDORED.md)
data/themes/             the bundled theme pack, compiled in as a GResource
tests/                   doctest suites for the GTK-free modules
scripts/build.sh
```

Everything except `app_window`, `theme_apply` and `main` is GTK-free and
tested directly.

## Licence

GPL-3.0; see `LICENSE`. Third-party components and their licences are listed
in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), with full texts in
`licenses/`.
