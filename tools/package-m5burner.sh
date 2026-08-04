#!/usr/bin/env bash
# Build the deterministic M5Burner component directory and archive from validated
# ESP32-S3 images. All live outputs are replaced as one recoverable transaction.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SRC="$ROOT/build"
OUT="$ROOT/firmware/cardputer"
ARCHIVE="$SRC/hotspot-arcade-cardputer-m5burner.zip"
LOCK="$ROOT/tools/toolchain.lock.json"
CONFIG="$ROOT/tools/arduino-cli.yaml"

node tools/validate-release.mjs
for artifact in \
  "$SRC/hotspot-arcade-cardputer.ino.bootloader.bin" \
  "$SRC/hotspot-arcade-cardputer.ino.partitions.bin" \
  "$SRC/hotspot-arcade-cardputer.ino.bin"; do
  [[ -s "$artifact" ]] || { echo "required build artifact is missing or empty: $artifact" >&2; exit 1; }
done

ARDUINO_CLI="${ARDUINO_CLI:-$(command -v arduino-cli 2>/dev/null || true)}"
[[ -n "$ARDUINO_CLI" ]] || { echo "arduino-cli is missing; run tools/bootstrap.sh" >&2; exit 2; }
DATA_DIR="$($ARDUINO_CLI --config-file "$CONFIG" config get directories.data)"
[[ "$DATA_DIR" = /* ]] || DATA_DIR="$ROOT/$DATA_DIR"
BOOT_RELATIVE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["arduino"]["bootApp0"]["relativeToData"])' "$LOCK")"
BOOT_EXPECTED_SHA="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["arduino"]["bootApp0"]["sha256"])' "$LOCK")"
BOOT_EXPECTED_SIZE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["arduino"]["bootApp0"]["size"])' "$LOCK")"
BOOT_APP0="$DATA_DIR/$BOOT_RELATIVE"
[[ -s "$BOOT_APP0" ]] || { echo "locked boot_app0.bin is missing: $BOOT_APP0" >&2; exit 2; }
[[ "$(wc -c < "$BOOT_APP0" | tr -d ' ')" == "$BOOT_EXPECTED_SIZE" ]] || { echo "boot_app0.bin size does not match lock" >&2; exit 2; }
if command -v shasum >/dev/null 2>&1; then
  BOOT_ACTUAL_SHA="$(shasum -a 256 "$BOOT_APP0" | awk '{print $1}')"
else
  BOOT_ACTUAL_SHA="$(sha256sum "$BOOT_APP0" | awk '{print $1}')"
fi
[[ "$BOOT_ACTUAL_SHA" == "$BOOT_EXPECTED_SHA" ]] || { echo "boot_app0.bin hash does not match lock" >&2; exit 2; }
if ! "$ARDUINO_CLI" --config-file "$CONFIG" core list | awk \
  '$1 == "esp32:esp32" && $2 == "3.3.11" { found = 1 } END { exit !found }'; then
  echo "project-local esp32:esp32@3.3.11 is not installed" >&2
  exit 2
fi

node tools/validate-images.mjs --build "$SRC" --boot-app "$BOOT_APP0"

mkdir -p "$ROOT/firmware" "$ROOT/.cache"
STAGE="$(mktemp -d "$ROOT/firmware/.cardputer-stage.XXXXXX")"
ARCHIVE_STAGE="$(mktemp -d "$ROOT/.cache/m5archive.XXXXXX")"
BACKUP="$ROOT/firmware/.cardputer-previous.$$"
ARCHIVE_BACKUP="$ROOT/.cache/m5archive-previous.$$"
cleanup() {
  if [[ -n "${STAGE:-}" && -d "$STAGE" ]]; then rm -rf "$STAGE"; fi
  if [[ -n "${ARCHIVE_STAGE:-}" && -d "$ARCHIVE_STAGE" ]]; then rm -rf "$ARCHIVE_STAGE"; fi
}
trap cleanup EXIT

cp "$SRC/hotspot-arcade-cardputer.ino.bootloader.bin" "$STAGE/bootloader_0x0.bin"
cp "$SRC/hotspot-arcade-cardputer.ino.partitions.bin" "$STAGE/partitions_0x8000.bin"
cp "$BOOT_APP0" "$STAGE/boot_app0_0xe000.bin"
cp "$SRC/hotspot-arcade-cardputer.ino.bin" "$STAGE/hotspot-arcade_0x10000.bin"
TZ=UTC touch -t 198001010000 "$STAGE"/*.bin
ARCHIVE_NEW="$ARCHIVE_STAGE/hotspot-arcade-cardputer-m5burner.zip"
(
  cd "$STAGE"
  zip -X -q "$ARCHIVE_NEW" \
    bootloader_0x0.bin \
    partitions_0x8000.bin \
    boot_app0_0xe000.bin \
    hotspot-arcade_0x10000.bin
)

had_out=false
had_archive=false
if [[ -d "$OUT" ]]; then
  mv "$OUT" "$BACKUP"
  had_out=true
fi
if [[ -f "$ARCHIVE" ]]; then
  if ! mv "$ARCHIVE" "$ARCHIVE_BACKUP"; then
    if $had_out; then mv "$BACKUP" "$OUT"; fi
    exit 3
  fi
  had_archive=true
fi
if mv "$STAGE" "$OUT" && mv "$ARCHIVE_NEW" "$ARCHIVE"; then
  STAGE=""
  rm -rf "$BACKUP"
  rm -f "$ARCHIVE_BACKUP"
else
  if [[ -d "$OUT" ]]; then rm -rf "$OUT"; fi
  if [[ -f "$ARCHIVE" ]]; then rm -f "$ARCHIVE"; fi
  if $had_out && [[ -d "$BACKUP" ]]; then mv "$BACKUP" "$OUT"; fi
  if $had_archive && [[ -f "$ARCHIVE_BACKUP" ]]; then mv "$ARCHIVE_BACKUP" "$ARCHIVE"; fi
  exit 3
fi

echo "packaged deterministic M5Burner components and archive:"
ls -l "$OUT" "$ARCHIVE"
