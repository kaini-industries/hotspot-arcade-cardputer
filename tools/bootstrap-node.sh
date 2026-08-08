#!/usr/bin/env bash
# Install the exact checksum-pinned Node runtime used by local/CI scripts.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$ROOT/tools/toolchain.lock.json"
TOOLS_ROOT="$ROOT/.tools"
DOWNLOADS="$ROOT/.cache/tool-downloads"
mkdir -p "$TOOLS_ROOT" "$DOWNLOADS"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) TARGET="darwin-arm64" ;;
  Linux-x86_64) TARGET="linux-x64" ;;
  *) echo "unsupported Node bootstrap host: $(uname -s)-$(uname -m)" >&2; exit 2 ;;
esac

json_value() {
  python3 -c 'import json,sys; value=json.load(open(sys.argv[1]));
for key in sys.argv[2].split("."): value=value[key]
print(value)' "$LOCK" "$1"
}

verify_sha256() {
  if command -v shasum >/dev/null 2>&1; then
    printf '%s  %s\n' "$1" "$2" | shasum -a 256 --check --status
  else
    printf '%s  %s\n' "$1" "$2" | sha256sum --check --status
  fi
}

VERSION="$(json_value node.version)"
URL="$(json_value "node.archives.$TARGET.url")"
FILE="$(json_value "node.archives.$TARGET.file")"
SHA="$(json_value "node.archives.$TARGET.sha256")"
MEMBER="$(json_value "node.archives.$TARGET.member")"
ARCHIVE="$DOWNLOADS/$FILE"
if [[ ! -f "$ARCHIVE" ]] || ! verify_sha256 "$SHA" "$ARCHIVE"; then
  curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
    --output "$ARCHIVE.tmp" "$URL"
  verify_sha256 "$SHA" "$ARCHIVE.tmp"
  mv "$ARCHIVE.tmp" "$ARCHIVE"
fi
verify_sha256 "$SHA" "$ARCHIVE" || { echo "Node archive hash mismatch" >&2; exit 3; }

UNPACK="$(mktemp -d "$TOOLS_ROOT/node.XXXXXX")"
trap 'rm -rf "$UNPACK"' EXIT
tar -xJf "$ARCHIVE" -C "$UNPACK" "$MEMBER"
[[ -x "$UNPACK/$MEMBER/bin/node" ]] || { echo "Node archive has no executable" >&2; exit 3; }

NODE_NEXT="$TOOLS_ROOT/node.next.$$"
NODE_PREVIOUS="$TOOLS_ROOT/node.previous.$$"
mv "$UNPACK/$MEMBER" "$NODE_NEXT"
if [[ -d "$TOOLS_ROOT/node" ]]; then mv "$TOOLS_ROOT/node" "$NODE_PREVIOUS"; fi
if mv "$NODE_NEXT" "$TOOLS_ROOT/node"; then
  rm -rf "$NODE_PREVIOUS"
else
  if [[ -d "$NODE_PREVIOUS" ]]; then mv "$NODE_PREVIOUS" "$TOOLS_ROOT/node"; fi
  exit 3
fi

export PATH="$TOOLS_ROOT/node/bin:$PATH"
[[ "$(node --version)" == "v$VERSION" ]] || { echo "Node version mismatch" >&2; exit 3; }
if [[ -n "${GITHUB_PATH:-}" ]]; then printf '%s\n' "$TOOLS_ROOT/node/bin" >> "$GITHUB_PATH"; fi
echo "locked Node v$VERSION is ready under .tools/node"
