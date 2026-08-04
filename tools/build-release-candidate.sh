#!/usr/bin/env bash
# Build and validate one complete release candidate in the current checkout.
# Reproducibility is established by running this script in two detached worktrees.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAG=""
if [[ "${1:-}" == "--tag" ]]; then
  TAG="${2:-}"
  [[ -n "$TAG" ]] || { echo "--tag requires a value" >&2; exit 2; }
  shift 2
fi
[[ $# -eq 0 ]] || { echo "usage: tools/build-release-candidate.sh [--tag vX.Y.Z]" >&2; exit 2; }
[[ "${SOURCE_DATE_EPOCH:-}" =~ ^(0|[1-9][0-9]*)$ ]] || {
  echo "SOURCE_DATE_EPOCH must be set to integer seconds" >&2
  exit 2
}

node tools/gen-assets.mjs --check
tools/build.sh
node tools/gen-assets.mjs --check
tools/package-m5burner.sh
node tools/generate-sbom.mjs build
node tools/validate-sbom.mjs build
if [[ -n "$TAG" ]]; then
  node tools/package-release.mjs --tag "$TAG"
else
  node tools/package-release.mjs
fi
node tools/release-hashes.mjs >/dev/null
