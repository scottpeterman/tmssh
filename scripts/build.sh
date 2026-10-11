#!/usr/bin/env bash
# scripts/build.sh -- configure, build and test tmssh.
#
#   scripts/build.sh                    debug-optimized build + tests
#   scripts/build.sh --release          release build in build-release/
#   scripts/build.sh --clean            wipe the build dir first
#   scripts/build.sh --no-tests         skip tests (doctest is not fetched)
#   scripts/build.sh --deps             apt-install the toolchain first (sudo)
#
# Flags combine: scripts/build.sh --clean --release
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILDTYPE="debugoptimized"
BUILD_DIR="$ROOT/build"
CLEAN=0
TESTS=1
DEPS=0

# libgtk-4-dev is listed now so --deps is a one-time step; the session model
# itself only needs yaml-cpp.
APT_PACKAGES=(build-essential git pkg-config meson ninja-build libyaml-cpp-dev libgtk-4-dev)

usage() {
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
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
        -h|--help)  usage; exit 0 ;;
        *)          usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

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

echo "==> meson $meson_version, yaml-cpp $(pkg-config --modversion yaml-cpp), $(c++ --version | head -1)"

# --- configure ---------------------------------------------------------------

if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "==> removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

tests_opt="true"
[[ $TESTS -eq 1 ]] || tests_opt="false"

opts=(--buildtype="$BUILDTYPE" -Dtests="$tests_opt")

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

echo "==> done: $BUILD_DIR"
