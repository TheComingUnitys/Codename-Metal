#!/usr/bin/env bash
# Build bgfx (Metal renderer only) as static libraries for macOS/arm64.
#
# bgfx upstream expects `bx` and `bimg` as *siblings* of the bgfx directory
# (see bgfx-master/makefile:27 `BX_DIR?=../bx` and
#  bgfx-master/scripts/genie.lua:179 `BIMG_DIR = BGFX_DIR/../bimg`).
#
# This script therefore builds out-of-tree: it creates a scratch directory with
# symlinks named bgfx/bx/bimg and runs GENie + make there, so the Codename-Metal
# workspace keeps its two real folders.
#
# Result: <scratch>/bgfx/.build/osx-arm64/bin/libbgfxRelease.a + libbxRelease.a

set -euo pipefail

WORKSPACE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BGFX_SRC="$WORKSPACE/bgfx-master"
SCRATCH="${BGFX_BUILD_DIR:-$HOME/bgfx-build}"
CONFIG="${BGFX_CONFIG:-release}"

if [ ! -d "$BGFX_SRC" ]; then
	echo "error: $BGFX_SRC not found" >&2
	exit 2
fi

# bx / bimg are git submodules of bgfx upstream; fetch them if missing.
if [ ! -d "$BGFX_SRC/bx" ]; then
	echo "cloning bx ..."
	git clone --depth 1 --recursive https://github.com/bkaradzic/bx.git "$BGFX_SRC/bx"
fi
if [ ! -d "$BGFX_SRC/bimg" ]; then
	echo "cloning bimg ..."
	git clone --depth 1 --recursive https://github.com/bkaradzic/bimg.git "$BGFX_SRC/bimg"
fi

mkdir -p "$SCRATCH"
ln -sfn "$BGFX_SRC"      "$SCRATCH/bgfx"
ln -sfn "$BGFX_SRC/bx"   "$SCRATCH/bx"
ln -sfn "$BGFX_SRC/bimg" "$SCRATCH/bimg"

# GENie resolves BGFX_DIR through the symlink, so `../bx` / `../bimg` must also
# exist next to the *real* bgfx directory.
ln -sfn "$BGFX_SRC/bx"   "$WORKSPACE/bx"
ln -sfn "$BGFX_SRC/bimg" "$WORKSPACE/bimg"

cd "$BGFX_SRC"

# bgfx alone is not enough: bgfx.cpp calls into bimg (imageParse/getBlockInfo), so
# libbimgRelease.a and libbimg_decodeRelease.a must exist and be linked too.
# BGFX_ARCHS lets a single-architecture build skip the other one (the macOS app is
# arm64-only here), which halves the rebuild time after a bgfx update.
for arch in ${BGFX_ARCHS:-osx-arm64 osx-x64}; do
	echo "=== generating $arch ==="
	MACOSX_DEPLOYMENT_TARGET=11.0 ./bx/tools/bin/darwin/genie --gcc=$arch gmake

	if [ "$arch" = "osx-x64" ]; then
		# genie bakes `-target x86_64-apple-macos10.13.6 -Werror`; the current SDK's
		# libc++ emits a deprecation #warning for that target, which -Werror turns
		# into a build failure.
		sed -i "" 's/-target x86_64-apple-macos10\.13\.6/-target x86_64-apple-macos11.0/g; s/ -Werror / -Wno-error /g' \
			.build/projects/gmake-$arch/*.make
	fi

	echo "=== building $arch ==="
	MACOSX_DEPLOYMENT_TARGET=11.0 make -C .build/projects/gmake-$arch config="$CONFIG" \
		bgfx bimg bimg_decode -j"$(sysctl -n hw.ncpu)"
done

echo
echo "built:"
ls -la .build/osx-arm64/bin/ 2>/dev/null | sed 's/^/  /'
