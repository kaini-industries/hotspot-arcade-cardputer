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

if [ "${1:-}" = "--tsan" ]; then
  "$CLANG" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=thread -fno-omit-frame-pointer \
    -Itests/native/include -Ihotspot-arcade-cardputer \
    tests/native/test_async_queue.cpp -o .cache/native/test_async_queue_tsan
  .cache/native/test_async_queue_tsan
  exit 0
fi

build_and_run() {
  local name="$1"
  "$CLANG" -std=c++17 -Wall -Wextra -Werror -pedantic -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itests/native/include -Ihotspot-arcade-cardputer \
    "tests/native/${name}.cpp" -o ".cache/native/${name}"
  ".cache/native/${name}"
}

build_and_run test_config
build_and_run test_active_nvs
build_and_run test_history
build_and_run test_host
build_and_run test_ap_reconnect
build_and_run test_ssid_transaction
build_and_run test_network_policy
build_and_run test_ws_flow_policy
build_and_run test_event_format
build_and_run test_ui_text
build_and_run test_async_queue
build_and_run test_device
