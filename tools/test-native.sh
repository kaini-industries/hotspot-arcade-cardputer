#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p .cache/native

NATIVE_NODE="${NODE:-}"
if [[ -z "$NATIVE_NODE" ]]; then
  if [[ -x "$ROOT/.tools/node/bin/node" ]]; then
    NATIVE_NODE="$ROOT/.tools/node/bin/node"
  else
    NATIVE_NODE="$(command -v node)"
  fi
fi
"$NATIVE_NODE" tools/gen-assets.mjs

CLANG="${CXX:-clang++}"

build_and_run() {
  local name="$1"
  "$CLANG" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itests/native/include -Ihotspot-arcade-cardputer \
    "tests/native/${name}.cpp" -o ".cache/native/${name}"
  ".cache/native/${name}"
}

build_and_run test_host
build_and_run test_ap_reconnect
build_and_run test_ssid_transaction
build_and_run test_network_policy
build_and_run test_ws_flow_policy
build_and_run test_event_format
