# tmssh

Tiny Managed SSH.

A small GTK 4 session-manager terminal for Linux: a session tree, tabs, and
ssh sessions on the [gtkpyte](https://github.com/scottpeterman/gtkpyte)
widget, plus local shells. Sessions live in one YAML file you edit by hand.
No vault, no telnet, no serial: ssh is the system client, so your
`~/.ssh/config`, agent and keys do the work.

## AppImage

Each release has `tmssh-<version>-x86_64.AppImage`, which carries GTK 4 and
its libraries; only the `ssh` client comes from the system.

```
chmod +x tmssh-0.1.0-x86_64.AppImage
```

```
./tmssh-0.1.0-x86_64.AppImage
```

AppImages mount themselves with FUSE. Where that is missing (a fresh Crostini
container, for one), either install it once or run the image without it:

```
sudo apt install -y libfuse2
```

```
./tmssh-0.1.0-x86_64.AppImage --appimage-extract-and-run
```

An AppImage runs on systems whose glibc is at least as new as the one it was
built on; the release notes say which. `scripts/appimage.sh` builds one from
a checkout into `dist/`.

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
| Ctrl+Shift+A | select all |
| Ctrl+= / Ctrl+- / Ctrl+0 | zoom in / out / normal size (Cmd on macOS) |
| Ctrl+scroll | zoom |
| Ctrl+Shift+L | start or stop logging the current tab |

Right-click in a terminal for Copy, Paste, Select All, zoom, logging,
Reconnect and Close Tab. The right button always opens the menu; a
full-screen program that asks for the mouse gets the left and middle
buttons only.

Zoom applies to every tab at once and is kept between runs (`font_zoom`
in the settings). A session with its own `font_size` zooms from that size.

When an ssh session ends, its tab stays open with the exit status, so an
authentication or connection error stays readable. Ctrl+Shift+R runs it
again.

## Session logs

**Log This Tab** in the menu (or Ctrl+Shift+L, or right-click → Log
Session) writes the tab's output to a new file in `~/tmssh-logs/`, named
for the session and the time: `edge1-01.fra1_20261012-014000.log`. A red dot
on the tab shows it is logging; its tooltip gives the file. **Log New SSH
Sessions** logs every ssh session from the moment it opens, and **Open Log
Folder** opens the directory.

Logs are plain text: colours and terminal control are stripped, and the
line editing a terminal does in place (carriage return, backspace,
erase-line) is applied first, so pager prompts such as `--More--` and
command-line edits do not appear and a line reads as it did on screen.
Full-screen programs such as vi or top do not reduce to lines and log
messily.

```
# tmssh session log: edge1-01.fra1
# target: admin@10.20.0.1
# started 2026-10-12 01:40:00 -0600
# tmssh: connecting 2026-10-12 01:40:00 -0600: ssh -l admin -- 10.20.0.1
admin@edge1-01.fra1> show bgp summary | no-more
...
# tmssh: session ended 2026-10-12 02:15:12 -0600 (exit 0)
# tmssh: log closed 2026-10-12 02:15:20 -0600
```

Reconnecting keeps writing to the same file, with a `connecting` line.
Only output is logged; typed input appears through the device's echo, so
passwords typed at a prompt that does not echo are never written. Files
are created mode 0600 (and the directory 0700 when tmssh creates it),
since a log holds whatever the device printed, configuration included. A
file is never appended to: two logs started in the same second get `-2`,
`-3`. `log_dir` in the settings moves them.

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
| `~/.config/tmssh/settings.yaml` | window, theme, font, zoom, scrollback, window size, logging |
| `~/tmssh-logs/` | session logs (`log_dir` in the settings) |
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

Folders are one level deep. Serial and telnet sessions (from a PathfinderSSH
file) are skipped on load and counted in the import summary. Keys tmssh
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
  session_log.{h,cpp}    session logs: terminal output to plain text, log files
  ssh_command.{h,cpp}    a session as an ssh command line
  settings.{h,cpp}       settings.yaml
  themes.{h,cpp}         theme files, colour parsing, the theme set
  theme_apply.{h,cpp}    themes onto the widget, app dark/light, the bundled pack
  app_window.{h,cpp}     the window: session tree, tabs, menus, dialogs
  main.cpp               options and startup
gtkpyte/                 the terminal widget, vendored (see gtkpyte/VENDORED.md)
data/themes/             the bundled theme pack, compiled in as a GResource
tests/                   doctest suites for the GTK-free modules
scripts/build.sh         build, test, install
scripts/appimage.sh      build the AppImage into dist/
```

Everything except `app_window`, `theme_apply` and `main` is GTK-free and
tested directly.

## Licence

GPL-3.0; see `LICENSE`. Third-party components and their licences are listed
in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), with full texts in
`licenses/`.
