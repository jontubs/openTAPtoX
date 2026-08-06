#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARDUINO_CLI_BIN="${ARDUINO_CLI_BIN:-arduino-cli}"
BUILD_ROOT="${BUILD_ROOT:-/tmp/opentaptox_repo_check}"
COMMON_LIB="$REPO_DIR/firmware/common"

fail() {
  echo "repo check failed: $1" >&2
  exit 1
}

require_command() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || fail "missing required command: $cmd"
}

compile_target() {
  local fqbn="$1"
  local sketch_dir="$2"
  local build_dir="$3"

  rm -rf "$build_dir"
  echo "[build] $fqbn -> $sketch_dir"
  "$ARDUINO_CLI_BIN" compile \
    --build-path "$build_dir" \
    --libraries "$COMMON_LIB" \
    --fqbn "$fqbn" \
    "$sketch_dir"
}

require_command "$ARDUINO_CLI_BIN"
require_command bash
require_command python3

echo "[check] public-tree privacy"
python3 "$REPO_DIR/tools/check_public_tree.py"

echo "[check] power semantics"
bash "$REPO_DIR/tools/check_power_semantics.sh"

compile_target "esp32:esp32:esp32c6" \
  "$REPO_DIR/firmware/openTAPtoX_esp32c6" \
  "$BUILD_ROOT/esp32c6"

compile_target "esp32:esp32:esp32c3" \
  "$REPO_DIR/firmware/openTAPtoX_esp32c6" \
  "$BUILD_ROOT/esp32c3"

echo "repo checks OK"
