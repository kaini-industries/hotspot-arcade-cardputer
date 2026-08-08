#!/usr/bin/env bash
# Build and validate one complete release candidate in the current checkout.
# Reproducibility is established by running this script in two detached worktrees.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TAG=""
CANDIDATE=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --tag)
      TAG="${2:-}"
      [[ -n "$TAG" ]] || { echo "--tag requires a value" >&2; exit 2; }
      shift 2
      ;;
    --candidate)
      CANDIDATE=true
      shift
      ;;
    *)
      echo "usage: tools/build-release-candidate.sh --tag vX.Y.Z [--candidate]" >&2
      exit 2
      ;;
  esac
done
if [[ -z "$TAG" ]]; then
  echo "--tag is required; use --candidate for an unpublished vX.Y.Z-rc.N build" >&2
  exit 2
fi
[[ "${SOURCE_DATE_EPOCH:-}" =~ ^(0|[1-9][0-9]*)$ ]] || {
  echo "SOURCE_DATE_EPOCH must be set to integer seconds" >&2
  exit 2
}

SOURCE_STATUS="$(git status --porcelain=v1 --untracked-files=all)"
if [[ -n "$SOURCE_STATUS" ]]; then
  echo "release source checkout is dirty; commit or remove every tracked and untracked change" >&2
  exit 3
fi
SOURCE_COMMIT="$(git rev-parse --verify 'HEAD^{commit}')"
[[ "$SOURCE_COMMIT" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid source commit" >&2; exit 3; }
if [[ -n "$TAG" && "$CANDIDATE" == false ]]; then
  TAG_COMMIT="$(git rev-parse --verify "refs/tags/$TAG^{commit}" 2>/dev/null)" || {
    echo "final release tag does not exist: $TAG" >&2
    exit 3
  }
  [[ "$TAG_COMMIT" == "$SOURCE_COMMIT" ]] || {
    echo "final release tag $TAG does not resolve to HEAD" >&2
    exit 3
  }
fi

# Generated sketch headers are intentionally ignored and therefore absent from a
# fresh detached release worktree. Materialize them from the locked vendor/content
# inputs before the build; build.sh regenerates them and the final check proves the
# second pass was identical.
node tools/gen-assets.mjs
tools/build.sh
node tools/gen-assets.mjs --check
tools/package-m5burner.sh
node tools/generate-sbom.mjs build
node tools/validate-sbom.mjs build
if [[ "$CANDIDATE" == true ]]; then
  node tools/package-release.mjs --tag "$TAG" --candidate
else
  node tools/package-release.mjs --tag "$TAG"
fi
node tools/release-hashes.mjs >/dev/null
