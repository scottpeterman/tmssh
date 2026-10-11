#!/usr/bin/env bash
# scripts/appimage.sh -- build dist/tmssh-<version>-x86_64.AppImage.
#
#   scripts/appimage.sh
#
# The AppImage carries GTK 4 and everything under it, so it runs without
# installing anything except the ssh client it calls. It runs on systems whose
# glibc is the same as, or newer than, the one it was built on: build on the
# oldest system you want to reach (Debian 12 reaches the most).
#
# Needs the build dependencies (scripts/build.sh --deps), curl, and network
# access the first time: linuxdeploy and its GTK plugin are fetched into
# .appimage-tools/ and reused after that.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/.appimage-tools"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
DIST="$ROOT/dist"
APP_ID="io.github.scottpeterman.tmssh"

die() {
    echo "appimage.sh: $*" >&2
    exit 1
}

[[ "$(uname -m)" == "x86_64" ]] || die "only x86_64 is set up here (uname -m: $(uname -m))"
command -v curl >/dev/null || die "curl is needed to fetch linuxdeploy"

VERSION="$(sed -n "s/^  version : '\(.*\)',$/\1/p" "$ROOT/meson.build" | head -1)"
[[ -n "$VERSION" ]] || die "could not read the version from meson.build"
OUT_NAME="tmssh-$VERSION-x86_64.AppImage"

# --- tools ---------------------------------------------------------------------

mkdir -p "$TOOLS"
fetch() {
    local url="$1" dest="$2"
    if [[ ! -x "$dest" ]]; then
        echo "==> fetching $(basename "$dest")"
        curl -fsSL -o "$dest.part" "$url"
        chmod +x "$dest.part"
        mv "$dest.part" "$dest"
    fi
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" "$TOOLS/linuxdeploy-x86_64.AppImage"
fetch "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" "$TOOLS/linuxdeploy-plugin-gtk.sh"

# linuxdeploy is itself an AppImage; this lets it run where FUSE is missing
# (a Crostini container, a CI container).
export APPIMAGE_EXTRACT_AND_RUN=1
export PATH="$TOOLS:$PATH"

# --- build into an AppDir ----------------------------------------------------------

echo "==> building tmssh $VERSION"
rm -rf "$BUILD"
meson setup "$BUILD" "$ROOT" --buildtype=release --prefix=/usr -Dtests=false >/dev/null
meson compile -C "$BUILD"
DESTDIR="$APPDIR" meson install -C "$BUILD" >/dev/null

# The installed launcher entry carries an absolute path for ~/.local installs;
# inside the AppImage it must be the bare name.
sed -i 's|^Exec=.*|Exec=tmssh|' "$APPDIR/usr/share/applications/$APP_ID.desktop"

# --- bundle GTK ----------------------------------------------------------------------

echo "==> bundling libraries"
deploy_args=(
    --appdir "$APPDIR"
    --plugin gtk
    --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop"
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg"
    --executable "$APPDIR/usr/bin/tmssh"
)
DEPLOY_GTK_VERSION=4 linuxdeploy-x86_64.AppImage "${deploy_args[@]}"

# The GTK plugin's start-up hook exports GTK_THEME=Adwaita:<variant>, which
# would lock the window to one variant. tmssh drops it at start-up and pins
# Adwaita itself (release_appimage_theme_pin in src/theme_apply.h), so the
# hook is left as the plugin wrote it.

# --- pack ------------------------------------------------------------------------------

echo "==> packing $OUT_NAME"
mkdir -p "$DIST"
(
    cd "$BUILD"
    OUTPUT="$OUT_NAME" linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --output appimage
    mv "$OUT_NAME" "$DIST/"
)

glibc="$(ldd --version | head -1 | awk '{print $NF}')"
echo "==> $DIST/$OUT_NAME ($(du -h "$DIST/$OUT_NAME" | cut -f1))"
echo "==> built against glibc $glibc: runs on systems with glibc $glibc or newer"
