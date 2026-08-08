#!/usr/bin/env bash
# Build the Cardputer firmware.
#
#   tools/build.sh          # regenerate baked assets + compile
#   tools/build.sh --deps   # bootstrap the locked project-local toolchain first
#
# Output: build/ (see README for the two install routes and their addresses).
set -euo pipefail

cd "$(dirname "$0")/.."

# The Cardputer v1 is an 8MB ESP32-S3 with no PSRAM. The board's DEFAULT partition
# scheme is the 4MB one with a 1.2MB app slot, which this firmware does not fit in.
FQBN="esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB"

if [[ "${1:-}" == "--deps" ]]; then
  tools/bootstrap.sh
  shift
fi
[[ $# -eq 0 ]] || { echo "usage: tools/build.sh [--deps]" >&2; exit 2; }

ARDUINO_CLI="${ARDUINO_CLI:-$(command -v arduino-cli 2>/dev/null || true)}"
[[ -n "$ARDUINO_CLI" ]] || { echo "arduino-cli is missing; run tools/bootstrap.sh" >&2; exit 3; }
[[ "$($ARDUINO_CLI version | awk '{print $3}')" == "1.5.1" ]] || {
  echo "arduino-cli 1.5.1 is required; run tools/bootstrap.sh" >&2
  exit 3
}

node tools/gen-assets.mjs

mkdir -p .cache
# ESP-IDF embeds the build directory in the ELF and then records that ELF hash in
# the application descriptor. A random mktemp path therefore makes otherwise
# identical firmware differ. Keep the staging path stable and replace the public
# build directory atomically only after every validation has passed.
BUILD_TMP="$PWD/.cache/build-stage"
rm -rf "$BUILD_TMP"
mkdir -p "$BUILD_TMP"
cleanup() {
  if [[ -n "$BUILD_TMP" && -d "$BUILD_TMP" ]]; then rm -rf "$BUILD_TMP"; fi
}
trap cleanup EXIT

"$ARDUINO_CLI" --config-file tools/arduino-cli.yaml compile \
  --clean \
  --fqbn "$FQBN" \
  --libraries vendor/libs \
  --build-path "$BUILD_TMP/work" \
  --output-dir "$BUILD_TMP" \
  hotspot-arcade-cardputer

node tools/trim-merged.mjs \
  "$BUILD_TMP/hotspot-arcade-cardputer.ino.merged.bin" \
  "$BUILD_TMP/hotspot-arcade-cardputer.full.bin"

node tools/check-build-budgets.mjs "$BUILD_TMP"

# Debug outputs are useful for budget checks but not release artifacts.
rm -f "$BUILD_TMP"/*.elf "$BUILD_TMP"/*.map
rm -rf "$BUILD_TMP/work"

BUILD_PREVIOUS="$PWD/.cache/build.previous.$$"
if [[ -d build ]]; then mv build "$BUILD_PREVIOUS"; fi
if mv "$BUILD_TMP" build; then
  BUILD_TMP=""
  rm -rf "$BUILD_PREVIOUS"
else
  if [[ -d "$BUILD_PREVIOUS" ]]; then mv "$BUILD_PREVIOUS" build; fi
  exit 4
fi

echo
echo "app image  (keeps M5Launcher, flash to 0x170000) : build/hotspot-arcade-cardputer.ino.bin"
echo "full image (replaces everything, flash to 0x0)   : build/hotspot-arcade-cardputer.full.bin"
