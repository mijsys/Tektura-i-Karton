#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
THEME="${KARTON_THEME:-light}"

meson setup "$BUILD_DIR" "$SCRIPT_DIR" --buildtype=debugoptimized
meson compile -C "$BUILD_DIR"
KARTON_THEME="$THEME" "$BUILD_DIR/karton-shell"
