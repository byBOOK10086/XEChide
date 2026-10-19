#!/bin/sh
# XEChide - prove that patches/xechide-core.patch still applies to the pinned
# stock KernelSU commit. Needs git; does not need a full clone.
set -eu

BASE=0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2
REPO=${REPO:-https://github.com/tiann/KernelSU.git}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

git init --quiet "$TMP/ksu"
cd "$TMP/ksu"
git remote add origin "$REPO"
git fetch --quiet --depth 1 origin "$BASE"
git checkout --quiet FETCH_HEAD
echo "base: $(git rev-parse HEAD)"

PATCH=$(cd "$(dirname "$0")/.." && pwd)/patches/xechide-core.patch
[ -f "$PATCH" ] || { echo "missing $PATCH" >&2; exit 1; }

git apply --check -v "$PATCH"
echo "patch applies cleanly"
