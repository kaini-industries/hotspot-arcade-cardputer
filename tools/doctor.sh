#!/usr/bin/env bash
# Read-only environment diagnosis. Use --ci to skip interactive/hardware-adjacent
# Mac prerequisites that are not needed by downstream CI.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
CI_MODE=false
[[ "${1:-}" == "--ci" ]] && CI_MODE=true
failures=0

ok() { printf 'ok   %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1" >&2; failures=$((failures + 1)); }
expect_version() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$actual" == "$expected" ]]; then
    ok "$label $actual"
  else
    bad "$label: expected $expected, found ${actual:-missing}"
  fi
}

if command -v git >/dev/null 2>&1; then ok "git $(git --version | awk '{print $3}')"; else bad "git is missing"; fi
if command -v python3 >/dev/null 2>&1; then ok "$(python3 --version)"; else bad "python3 is missing"; fi
if command -v node >/dev/null 2>&1; then
  expect_version node "v24.19.0" "$(node --version)"
else
  bad "node is missing (run nvm use)"
fi

CLI="${ARDUINO_CLI:-$(command -v arduino-cli 2>/dev/null || true)}"
if [[ -n "$CLI" ]]; then
  expect_version arduino-cli "1.5.1" "$($CLI version 2>/dev/null | awk '{print $3}')"
  if "$CLI" --config-file tools/arduino-cli.yaml core list 2>/dev/null | grep -Eq '^esp32:esp32[[:space:]]+3\.3\.11([[:space:]]|$)'; then
    ok "ESP32 core 3.3.11 (project-local)"
  else
    bad "project-local ESP32 core 3.3.11 is missing"
  fi
  for spec in 'IRremote 4.7.1' 'LibSSH-ESP32 5.8.0' 'M5GFX 0.2.26' 'M5Unified 0.2.19' 'M5Cardputer 1.1.1'; do
    name="${spec% *}"; version="${spec##* }"
    if "$CLI" --config-file tools/arduino-cli.yaml lib list 2>/dev/null | grep -F "$name" | grep -Fq "$version"; then
      ok "$name $version"
    else
      bad "$name $version is missing from the project-local Arduino user directory"
    fi
  done
else
  bad "arduino-cli is missing"
fi

if command -v esptool >/dev/null 2>&1; then
  expect_version esptool "5.3.1" "$(esptool version 2>/dev/null | awk 'NR==1 {sub(/^v/,"",$2); print $2}')"
else
  bad "esptool is missing"
fi

if ! $CI_MODE; then
  EMSCRIPTEN_VERSION_FILE="$ROOT/.tools/emsdk/upstream/emscripten/emscripten-version.txt"
  if [[ -f "$EMSCRIPTEN_VERSION_FILE" ]]; then
    expect_version emscripten "6.0.2" "$(tr -d '\"[:space:]' < "$EMSCRIPTEN_VERSION_FILE")"
  elif command -v emcc >/dev/null 2>&1; then
    expect_version emscripten "6.0.2" "$(emcc --version 2>&1 | awk '/^emcc / {print $NF; exit}')"
  else
    bad "Emscripten 6.0.2 is missing"
  fi
  if command -v actionlint >/dev/null 2>&1; then
    expect_version actionlint "1.7.12" "$(actionlint -version 2>/dev/null | awk 'NR == 1 {print; exit}')"
  else
    bad "actionlint is missing"
  fi
  if command -v gh >/dev/null 2>&1 && gh auth status -h github.com >/dev/null 2>&1; then
    ok "GitHub CLI authentication"
  else
    bad "GitHub CLI authentication needs: gh auth login -h github.com"
  fi
  for tool in shellcheck clang cmake ninja; do
    if command -v "$tool" >/dev/null 2>&1; then ok "$tool"; else bad "$tool is missing"; fi
  done
  UPSTREAM="$ROOT/../hotspot-arcade"
  if [[ -d "$UPSTREAM/.git" ]]; then ok "upstream clone $UPSTREAM"; else bad "upstream clone is missing at $UPSTREAM"; fi
fi

if node tools/verify-toolchain-lock.mjs >/dev/null 2>&1; then
  ok "toolchain lock/index agreement"
else
  bad "toolchain lock does not match the project indexes"
fi
((failures == 0)) || exit 1
