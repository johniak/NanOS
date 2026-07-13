#!/usr/bin/env bash
# fetch-node.sh — clone the pinned Node source for the NanOS Electron program (plan 02 Task 2.1).
#
# Per plan 02 Decisions: try Electron's Node fork at the pinned version first; if that tag is not
# published on the fork (electron/node versions on branches, not release tags), fall back to upstream
# nodejs/node at the SAME version and record the delta here. The node_version is pinned in
# manifest/electron-stack.lock and must match.
#
# Writes $SDK_WORK/node-src with NANOS_REVISION.txt recording the exact commit used.
set -eu

SDK_WORK="${SDK_WORK:-$HOME/Projects/nanos-sdk-work}"
DEST="$SDK_WORK/node-src"
LOCK="$(cd "$(dirname "$0")/../.." && pwd)/manifest/electron-stack.lock"

# node_version = "vX.Y.Z" from the lock (strip quotes/comment).
VER="$(sed -n 's/^node_version[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$LOCK" | head -1)"
[ -n "$VER" ] || { echo "FAIL: node_version not found in $LOCK"; exit 2; }
echo "fetch-node: pinned Node $VER (from manifest/electron-stack.lock)"

FORK="https://github.com/electron/node"
UPSTREAM="https://github.com/nodejs/node"

if [ -d "$DEST/.git" ]; then
	echo "fetch-node: $DEST already a git tree — leaving it (delete it to re-fetch)"
else
	# Prefer the Electron fork tag; fall back to upstream if the tag is absent on the fork.
	SRC="$FORK"
	if ! git ls-remote --tags "$FORK" "refs/tags/$VER" 2>/dev/null | grep -q "$VER"; then
		echo "fetch-node: electron/node has no tag $VER (it versions on branches) — falling back to upstream nodejs/node (plan 02 Decisions fallback)"
		SRC="$UPSTREAM"
	fi
	echo "fetch-node: shallow-cloning $SRC @ $VER -> $DEST"
	git clone --depth 1 --branch "$VER" "$SRC" "$DEST"
	echo "$SRC $VER" > "$DEST/NANOS_SOURCE.txt"
fi

cd "$DEST"
git rev-parse HEAD > NANOS_REVISION.txt
echo "fetch-node: HEAD = $(cat NANOS_REVISION.txt)"

# Record the component versions from the source tree (no build needed for these).
echo "fetch-node: component versions —"
node_v="$(sed -n 's/^#define NODE_VERSION "\(.*\)"/\1/p' src/node_version.h 2>/dev/null | head -1)"
v8_maj="$(sed -n 's/^#define V8_MAJOR_VERSION \(.*\)/\1/p' deps/v8/include/v8-version.h 2>/dev/null)"
v8_min="$(sed -n 's/^#define V8_MINOR_VERSION \(.*\)/\1/p' deps/v8/include/v8-version.h 2>/dev/null)"
v8_bld="$(sed -n 's/^#define V8_BUILD_NUMBER \(.*\)/\1/p' deps/v8/include/v8-version.h 2>/dev/null)"
v8_pat="$(sed -n 's/^#define V8_PATCH_LEVEL \(.*\)/\1/p' deps/v8/include/v8-version.h 2>/dev/null)"
uv_maj="$(sed -n 's/^#define UV_VERSION_MAJOR \(.*\)/\1/p' deps/uv/include/uv/version.h 2>/dev/null)"
uv_min="$(sed -n 's/^#define UV_VERSION_MINOR \(.*\)/\1/p' deps/uv/include/uv/version.h 2>/dev/null)"
uv_pat="$(sed -n 's/^#define UV_VERSION_PATCH \(.*\)/\1/p' deps/uv/include/uv/version.h 2>/dev/null)"
echo "  node   = ${node_v:-?}"
echo "  v8     = ${v8_maj:-?}.${v8_min:-?}.${v8_bld:-?}.${v8_pat:-?}"
echo "  libuv  = ${uv_maj:-?}.${uv_min:-?}.${uv_pat:-?}"
echo "fetch-node: done."
