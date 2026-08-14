#!/usr/bin/env bash
# Install the exact Emscripten simulator SDK in a project-local directory. The
# emsdk installer revision and release commit behind the locked version are
# locked; no global shell profile or SDK configuration is changed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
LOCK="$ROOT/tools/toolchain.lock.json"
EMSDK="$ROOT/.tools/emsdk"

json_value() {
  python3 -c 'import json,sys; value=json.load(open(sys.argv[1]));
for key in sys.argv[2].split("."): value=value[key]
print(value)' "$LOCK" "$1"
}

VERSION="$(json_value hostTools.emscripten.version)"
INSTALLER_COMMIT="$(json_value hostTools.emscripten.installerCommit)"
RELEASE_COMMIT="$(json_value hostTools.emscripten.sdkReleaseCommit)"

if [[ ! -d "$EMSDK/.git" ]]; then
  mkdir -p "$ROOT/.tools"
  git clone --filter=blob:none https://github.com/emscripten-core/emsdk.git "$EMSDK"
fi

git -C "$EMSDK" fetch --depth=1 origin "$INSTALLER_COMMIT"
git -C "$EMSDK" checkout --detach "$INSTALLER_COMMIT"
[[ "$(git -C "$EMSDK" rev-parse HEAD)" == "$INSTALLER_COMMIT" ]] || {
  echo "emsdk installer commit mismatch" >&2
  exit 3
}

RESOLVED="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["releases"][sys.argv[2]])' \
  "$EMSDK/emscripten-releases-tags.json" "$VERSION")"
[[ "$RESOLVED" == "$RELEASE_COMMIT" ]] || {
  echo "Emscripten $VERSION resolved to $RESOLVED instead of $RELEASE_COMMIT" >&2
  exit 3
}

"$EMSDK/emsdk" install "$VERSION"
"$EMSDK/emsdk" activate "$VERSION"
[[ "$(tr -d '\"[:space:]' < "$EMSDK/upstream/emscripten/emscripten-version.txt")" == "$VERSION" ]] || {
  echo "installed Emscripten version mismatch" >&2
  exit 3
}

node tools/verify-toolchain-lock.mjs
printf 'Emscripten %s is ready; run: source %s/emsdk_env.sh\n' "$VERSION" "$EMSDK"
