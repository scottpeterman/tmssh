#!/usr/bin/env bash
# scripts/build.sh -- configure, build and test tmssh; optionally install or run.
#
#   scripts/build.sh                    debug-optimized build + tests
#   scripts/build.sh --release          release build in build-release/
#   scripts/build.sh --clean            wipe the build dir first
#   scripts/build.sh --no-tests         skip tests (doctest is not fetched)
#   scripts/build.sh --deps             apt-install the toolchain first (sudo)
#   scripts/build.sh --install          install to ~/.local (bin + launcher entry)
#   scripts/build.sh --prefix /opt/gp   install somewhere else (implies --install)
#   scripts/build.sh --run -- --theme nord  build, then run tmssh with these args
#
# Flags combine: scripts/build.sh --release --install
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILDTYPE="debugoptimized"
BUILD_DIR="$ROOT/build"
CLEAN=0
TESTS=1
DEPS=0
INSTALL=0
PREFIX="$HOME/.local"
RUN=0
RUN_ARGS=()

APT_PACKAGES=(build-essential git pkg-config meson ninja-build libyaml-cpp-dev libgtk-4-dev)

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
    echo "build.sh: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)  BUILDTYPE="release"; BUILD_DIR="$ROOT/build-release" ;;
        --clean)    CLEAN=1 ;;
        --no-tests) TESTS=0 ;;
        --deps)     DEPS=1 ;;
        --install)  INSTALL=1 ;;
        --prefix)
            [[ $# -ge 2 ]] || die "--prefix needs a directory"
            PREFIX="$2"; INSTALL=1; shift ;;
        --prefix=*) PREFIX="${1#--prefix=}"; INSTALL=1 ;;
        --run)      RUN=1 ;;
        --)         shift; RUN_ARGS=("$@"); break ;;
        -h|--help)  usage; exit 0 ;;
        *)          usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

[[ "$PREFIX" = /* ]] || die "--prefix must be an absolute path"

# --- toolchain ---------------------------------------------------------------

if [[ $DEPS -eq 1 ]]; then
    command -v apt-get >/dev/null || die "--deps uses apt-get; install these by hand: ${APT_PACKAGES[*]}"
    echo "==> installing ${APT_PACKAGES[*]}"
    sudo apt-get update
    sudo apt-get install -y "${APT_PACKAGES[@]}"
fi

missing=()
for tool in meson ninja pkg-config c++ git; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
if [[ ${#missing[@]} -eq 0 ]] && ! pkg-config --atleast-version=4.8 gtk4; then
    missing+=("gtk4 >= 4.8 (libgtk-4-dev)")
fi
if [[ ${#missing[@]} -eq 0 ]] && ! pkg-config --atleast-version=0.7.0 yaml-cpp; then
    missing+=("yaml-cpp >= 0.7.0 (libyaml-cpp-dev)")
fi
if [[ ${#missing[@]} -gt 0 ]]; then
    die "missing: ${missing[*]} -- rerun with --deps, or: sudo apt install -y ${APT_PACKAGES[*]}"
fi

meson_version="$(meson --version)"
if ! printf '1.0.0\n%s\n' "$meson_version" | sort -V -C; then
    die "meson $meson_version is too old; 1.0.0 or newer is required"
fi

echo "==> meson $meson_version, gtk4 $(pkg-config --modversion gtk4), yaml-cpp $(pkg-config --modversion yaml-cpp), $(c++ --version | head -1)"

# --- configure ---------------------------------------------------------------

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "==> removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

tests_opt="true"
[[ $TESTS -eq 1 ]] || tests_opt="false"

opts=(--buildtype="$BUILDTYPE" -Dtests="$tests_opt" --prefix="$PREFIX")

if [[ -f "$BUILD_DIR/build.ninja" ]]; then
    echo "==> reconfiguring $BUILD_DIR ($BUILDTYPE)"
    meson configure "$BUILD_DIR" "${opts[@]}" >/dev/null
else
    # A directory left by a failed setup has no build.ninja; start it over.
    rm -rf "$BUILD_DIR"
    echo "==> configuring $BUILD_DIR ($BUILDTYPE)"
    meson setup "$BUILD_DIR" "$ROOT" "${opts[@]}"
fi

# --- build and test ----------------------------------------------------------

echo "==> compiling"
meson compile -C "$BUILD_DIR"

if [[ $TESTS -eq 1 ]]; then
    echo "==> testing"
    meson test -C "$BUILD_DIR" --print-errorlogs
fi

# --- install -----------------------------------------------------------------

if [[ $INSTALL -eq 1 ]]; then
    echo "==> installing to $PREFIX"
    if [[ -w "$PREFIX" || ( ! -e "$PREFIX" && -w "$(dirname "$PREFIX")" ) ]]; then
        meson install -C "$BUILD_DIR"
    else
        sudo meson install -C "$BUILD_DIR"
    fi
    apps_dir="$PREFIX/share/applications"
    if command -v update-desktop-database >/dev/null; then
        update-desktop-database "$apps_dir" 2>/dev/null || true
    fi
    echo "==> installed $PREFIX/bin/tmssh"
    echo "==> launcher entry $apps_dir/io.github.scottpeterman.tmssh.desktop"
fi

# --- run ---------------------------------------------------------------------

if [[ $RUN -eq 1 ]]; then
    echo "==> running tmssh ${RUN_ARGS[*]:-}"
    if [[ ${#RUN_ARGS[@]} -gt 0 ]]; then
        exec "$BUILD_DIR/tmssh" "${RUN_ARGS[@]}"
    fi
    exec "$BUILD_DIR/tmssh"
fi

echo "==> done: $BUILD_DIR/tmssh"
