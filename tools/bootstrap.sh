#!/usr/bin/env bash
# Install the locked Arduino CLI when needed, verify mutable indexes against the
# reviewed lock, and install the core/libraries into project-local directories.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

LOCK="$ROOT/tools/toolchain.lock.json"
CONFIG="$ROOT/tools/arduino-cli.yaml"
TOOLS_BIN="$ROOT/.tools/bin"
DOWNLOADS="$ROOT/.cache/tool-downloads"
mkdir -p "$TOOLS_BIN" "$DOWNLOADS"

json_value() {
  python3 -c 'import json,sys; value=json.load(open(sys.argv[1]));
for key in sys.argv[2].split("."): value=value[key]
print(value)' "$LOCK" "$1"
}

verify_sha256() {
  local expected="$1"
  local path="$2"
  if command -v shasum >/dev/null 2>&1; then
    printf '%s  %s\n' "$expected" "$path" | shasum -a 256 --check --status
  else
    printf '%s  %s\n' "$expected" "$path" | sha256sum --check --status
  fi
}

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) TARGET="darwin-arm64" ;;
  Linux-x86_64) TARGET="linux-x64" ;;
  *) echo "unsupported bootstrap host: $(uname -s)-$(uname -m)" >&2; exit 2 ;;
esac

CLI_VERSION="$(json_value hostTools.arduinoCli.version)"
CLI="$TOOLS_BIN/arduino-cli"
ARCHIVE_URL="$(json_value hostTools.arduinoCli.archives.$TARGET.url)"
ARCHIVE_FILE="$(json_value hostTools.arduinoCli.archives.$TARGET.file)"
ARCHIVE_SHA="$(json_value hostTools.arduinoCli.archives.$TARGET.sha256)"
ARCHIVE_PATH="$DOWNLOADS/$ARCHIVE_FILE"
if [[ ! -f "$ARCHIVE_PATH" ]] || ! verify_sha256 "$ARCHIVE_SHA" "$ARCHIVE_PATH"; then
  curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
    --output "$ARCHIVE_PATH.tmp" "$ARCHIVE_URL"
  verify_sha256 "$ARCHIVE_SHA" "$ARCHIVE_PATH.tmp"
  mv "$ARCHIVE_PATH.tmp" "$ARCHIVE_PATH"
fi
verify_sha256 "$ARCHIVE_SHA" "$ARCHIVE_PATH" || { echo "arduino-cli archive hash mismatch" >&2; exit 3; }
CLI_UNPACK="$(mktemp -d "$ROOT/.tools/arduino-cli.XXXXXX")"
trap 'rm -rf "$CLI_UNPACK"' EXIT
tar -xzf "$ARCHIVE_PATH" -C "$CLI_UNPACK" arduino-cli
[[ -x "$CLI_UNPACK/arduino-cli" ]] || { echo "arduino-cli archive has no executable" >&2; exit 3; }
mv "$CLI_UNPACK/arduino-cli" "$CLI"
[[ "$($CLI version | awk '{print $3}')" == "$CLI_VERSION" ]] || { echo "arduino-cli version mismatch" >&2; exit 3; }
export ARDUINO_CLI="$CLI"
export PATH="$TOOLS_BIN:$PATH"
if [[ -n "${GITHUB_PATH:-}" ]]; then printf '%s\n' "$TOOLS_BIN" >> "$GITHUB_PATH"; fi

"$CLI" --config-file "$CONFIG" core update-index
"$CLI" --config-file "$CONFIG" lib update-index
node tools/verify-toolchain-lock.mjs --target "$TARGET"

CORE_VERSION="$(json_value arduino.core.version)"
if ! "$CLI" --config-file "$CONFIG" core list | grep -Eq "^esp32:esp32[[:space:]]+$CORE_VERSION([[:space:]]|$)"; then
  "$CLI" --config-file "$CONFIG" core install "esp32:esp32@$CORE_VERSION"
fi

LOCKED_LIBRARIES=()
while IFS= read -r line; do LOCKED_LIBRARIES+=("$line"); done < <(
  python3 -c 'import json,sys
for item in json.load(open(sys.argv[1]))["arduino"]["libraries"]: print("{}@{}".format(item["name"], item["version"]))' "$LOCK"
)
"$CLI" --config-file "$CONFIG" lib install --no-deps "${LOCKED_LIBRARIES[@]}"

# M5GFX 0.2.26 is the latest registry release, but its Cardputer Advance
# autodetection overwrites the GPIO probe bits. Apply only the exact upstream
# fix accepted as M5GFX PR #233; the helper checks the release preimage, stages
# the result, and accepts an already-patched rerun.
node tools/arduino-library-patches.mjs

node tools/verify-toolchain-lock.mjs --target "$TARGET" --installed
echo "locked Arduino toolchain is ready under .cache/arduino"
