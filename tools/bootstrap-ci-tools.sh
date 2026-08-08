#!/usr/bin/env bash
# Install checksum-pinned CI executables without trusting runner-global tools.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$ROOT/tools/toolchain.lock.json"
TOOLS_BIN="$ROOT/.tools/bin"
DOWNLOADS="$ROOT/.cache/tool-downloads"
mkdir -p "$TOOLS_BIN" "$DOWNLOADS"

[[ "$(uname -s)-$(uname -m)" == "Linux-x86_64" ]] || {
  echo "bootstrap-ci-tools.sh supports the pinned linux-x64 CI host only" >&2
  exit 2
}

json_value() {
  python3 -c 'import json,sys; value=json.load(open(sys.argv[1]));
for key in sys.argv[2].split("."): value=value[key]
print(value)' "$LOCK" "$1"
}

verify_sha256() {
  printf '%s  %s\n' "$1" "$2" | sha256sum --check --status
}

download_locked() {
  local key="$1"
  local url file sha path
  url="$(json_value "hostTools.$key.archives.linux-x64.url")"
  file="$(json_value "hostTools.$key.archives.linux-x64.file")"
  sha="$(json_value "hostTools.$key.archives.linux-x64.sha256")"
  path="$DOWNLOADS/$file"
  if [[ ! -f "$path" ]] || ! verify_sha256 "$sha" "$path"; then
    curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
      --output "$path.tmp" "$url"
    verify_sha256 "$sha" "$path.tmp"
    mv "$path.tmp" "$path"
  fi
  verify_sha256 "$sha" "$path"
  printf '%s\n' "$path"
}

install_archive() {
  local key="$1"
  local executable="$2"
  local archive member unpack source
  archive="$(download_locked "$key")"
  member="$(json_value "hostTools.$key.archives.linux-x64.member")"
  unpack="$(mktemp -d "$ROOT/.tools/$executable.XXXXXX")"
  tar -xzf "$archive" -C "$unpack" "$member"
  source="$unpack/$member"
  [[ -f "$source" && ! -L "$source" ]] || { echo "$key archive member is invalid" >&2; exit 3; }
  install -m 0755 "$source" "$TOOLS_BIN/$executable.tmp"
  mv "$TOOLS_BIN/$executable.tmp" "$TOOLS_BIN/$executable"
  rm -rf "$unpack"
}

install_binary() {
  local key="$1"
  local executable="$2"
  local source
  source="$(download_locked "$key")"
  [[ -f "$source" && ! -L "$source" ]] || { echo "$key binary is invalid" >&2; exit 3; }
  install -m 0755 "$source" "$TOOLS_BIN/$executable.tmp"
  mv "$TOOLS_BIN/$executable.tmp" "$TOOLS_BIN/$executable"
}

if [[ $# -eq 0 ]]; then set -- actionlint syft cosign githubCli; fi
for tool in "$@"; do
  case "$tool" in
    actionlint) install_archive actionlint actionlint ;;
    syft) install_archive syft syft ;;
    cosign) install_binary cosign cosign ;;
    githubCli) install_archive githubCli gh ;;
    *) echo "unknown locked CI tool: $tool" >&2; exit 2 ;;
  esac
done

export PATH="$TOOLS_BIN:$PATH"
if [[ -n "${GITHUB_PATH:-}" ]]; then printf '%s\n' "$TOOLS_BIN" >> "$GITHUB_PATH"; fi

for tool in "$@"; do
  case "$tool" in
    actionlint)
      actual="$(actionlint -version)"
      actual="${actual%%$'\n'*}"
      expected="$(json_value hostTools.actionlint.version)"
      [[ "$actual" == "$expected" ]] || {
        echo "actionlint version mismatch: expected $expected, got $actual" >&2
        exit 4
      }
      ;;
    syft)
      syft version | grep -Eq "Version:[[:space:]]+$(json_value hostTools.syft.version)([[:space:]]|$)"
      ;;
    cosign)
      cosign version | grep -Eq "GitVersion:[[:space:]]+v$(json_value hostTools.cosign.version)([[:space:]]|$)"
      ;;
    githubCli)
      gh --version | head -n 1 | grep -Eq "^gh version $(json_value hostTools.githubCli.version)([[:space:]]|$)"
      ;;
  esac
done

echo "locked CI tools are ready under .tools/bin"
