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

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) TARGET="darwin-arm64" ;;
  Linux-x86_64) TARGET="linux-x64" ;;
  *) echo "unsupported bootstrap host: $(uname -s)-$(uname -m)" >&2; exit 2 ;;
esac

CLI_VERSION="$(json_value hostTools.arduinoCli.version)"
CLI="$TOOLS_BIN/arduino-cli"
if command -v arduino-cli >/dev/null 2>&1 && [[ "$(arduino-cli version | awk '{print $3}')" == "$CLI_VERSION" ]]; then
  CLI="$(command -v arduino-cli)"
elif [[ ! -x "$CLI" ]] || [[ "$($CLI version | awk '{print $3}')" != "$CLI_VERSION" ]]; then
  ARCHIVE_URL="$(json_value hostTools.arduinoCli.archives.$TARGET.url)"
  ARCHIVE_FILE="$(json_value hostTools.arduinoCli.archives.$TARGET.file)"
  ARCHIVE_SHA="$(json_value hostTools.arduinoCli.archives.$TARGET.sha256)"
  ARCHIVE_PATH="$DOWNLOADS/$ARCHIVE_FILE"
  curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
    --output "$ARCHIVE_PATH.tmp" "$ARCHIVE_URL"
  if command -v shasum >/dev/null 2>&1; then
    printf '%s  %s\n' "$ARCHIVE_SHA" "$ARCHIVE_PATH.tmp" | shasum -a 256 --check --status
  else
    printf '%s  %s\n' "$ARCHIVE_SHA" "$ARCHIVE_PATH.tmp" | sha256sum --check --status
  fi
  mv "$ARCHIVE_PATH.tmp" "$ARCHIVE_PATH"
  tar -xzf "$ARCHIVE_PATH" -C "$TOOLS_BIN" arduino-cli
fi
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

node tools/verify-toolchain-lock.mjs --target "$TARGET"
echo "locked Arduino toolchain is ready under .cache/arduino"
