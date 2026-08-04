#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p .cache/native

CLANG="${CXX:-clang++}"

build_and_run() {
  local name="$1"
  "$CLANG" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Itests/native/include -Ihotspot-arcade-cardputer \
    "tests/native/${name}.cpp" -o ".cache/native/${name}"
  ".cache/native/${name}"
}

build_and_run test_config
build_and_run test_active_nvs
