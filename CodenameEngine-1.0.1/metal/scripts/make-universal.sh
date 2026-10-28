#!/usr/bin/env bash
# Build a Universal (arm64 + x86_64) Codename Engine with the Metal renderer.
#
#   ./metal/scripts/make-universal.sh
#
# STATUS — verified:
#   * Universal `lime.ndll`  : WORKS.  lipo of the two hxcpp builds gives
#                              "x86_64 arm64" (21,246,272 bytes) and, installed into
#                              the .app, still reports at runtime
#                              `[cne-metal] bgfx renderer: Metal (Metal: yes)`.
#   * Universal executable   : the Intel pass of `lime build` currently still emits an
#                              arm64 binary.  The script detects this and ships the
#                              arm64 executable (safe) instead of faking a universal one.
#                              See README 19.3 for the remaining step.
#
# Host prerequisites:
#   * bgfx / bx / bimg / bimg_decode built for osx-x64
#     (`metal/scripts/build-bgfx.sh` builds both architectures).
#   * The HOST toolchain must stay arm64.  xcrun itself is arm64-only, so a Rosetta
#     x86_64 haxe/neko cannot drive this build:
#         xcrun: unable to load libxcrun ... (have 'arm64,arm64e', need 'x86_64')
#     Only the *compiler output* has to be x86_64.
#
# The Intel pass works because of `$HXCPP/toolchain/mac-toolchain.xml`:
# its `<section unless="HXCPP_ARCH">` is the official override point -- defining
# HXCPP_ARCH before it makes every `-arch` flag use that value.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
METAL_DIR="$(dirname "$HERE")"
PROJECT_DIR="${PROJECT_DIR:-$(cd "$METAL_DIR/.." && pwd)}"
BGFX_DIR="${BGFX_DIR:-$(cd "$PROJECT_DIR/.." && pwd)/bgfx-master}"
LIME_DIR="${LIME_DIR:-$HOME/haxelib-dev/lime-832}"
HXCPP="${HXCPP_DIR:-$HOME/haxelib/hxcpp/4,3,2}"
OUT="${OUT:-/tmp/cne-universal}"

APP="$PROJECT_DIR/export/release/macos/bin/CodenameEngine.app"
BIN="$APP/Contents/MacOS/CodenameEngine"
NDLL="$LIME_DIR/ndll/MacArm64/lime-64.ndll"
TOOLCHAIN="$HXCPP/toolchain/mac-toolchain.xml"
BUILD_XML="$LIME_DIR/project/Build.xml"

mkdir -p "$OUT"
say() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
die() { printf '\033[31merror:\033[0m %s\n' "$1" >&2; exit 1; }

[ -f "$BUILD_XML" ] || die "no Lime checkout at $LIME_DIR"
[ -d "$BGFX_DIR/.build/osx-x64/bin" ] || \
	die "missing Intel bgfx archives; run metal/scripts/build-bgfx.sh first"

cp "$TOOLCHAIN" "$OUT/mac-toolchain.xml.orig"
trap 'cp "$OUT/mac-toolchain.xml.orig" "$TOOLCHAIN"; sed -i "" "s|\.build/osx-x64/bin|.build/osx-arm64/bin|g" "$BUILD_XML" 2>/dev/null || true' EXIT

# ------------------------------------------------------------------ arm64 pass
say "pass 1/2: arm64"
sed -i "" 's|\.build/osx-x64/bin|.build/osx-arm64/bin|g' "$BUILD_XML"
rm -rf "$LIME_DIR/project/obj"
haxelib run lime rebuild mac -Dmetal
[ -f "$NDLL" ] || die "arm64 lime rebuild produced no $NDLL"
haxelib run lime build mac
cp "$NDLL" "$OUT/lime-64-arm64.ndll"
cp "$BIN"  "$OUT/CodenameEngine-arm64"
lipo -archs "$OUT/lime-64-arm64.ndll" "$OUT/CodenameEngine-arm64"

# ------------------------------------------------------------------ x86_64 pass
say "pass 2/2: x86_64"
sed -i "" 's|\.build/osx-arm64/bin|.build/osx-x64/bin|g' "$BUILD_XML"
rm -rf "$LIME_DIR/project/obj"
haxelib run lime rebuild mac -Dmetal -DCNE_X64
[ -f "$NDLL" ] || die "Intel lime rebuild produced no $NDLL"
cp "$NDLL" "$OUT/lime-64-x86_64.ndll"
lipo -archs "$OUT/lime-64-x86_64.ndll"

haxelib run lime build mac || true
cp "$BIN" "$OUT/CodenameEngine-intel-last"

# ------------------------------------------------------------------ merge
say "merging"
lipo -create "$OUT/lime-64-arm64.ndll" "$OUT/lime-64-x86_64.ndll" \
     -output "$OUT/lime-universal.ndll"
lipo -archs "$OUT/lime-universal.ndll"

if [ "$(lipo -archs "$OUT/CodenameEngine-intel-last")" = "arm64" ]; then
	echo
	echo "NOTE: the Intel pass of 'lime build' still produced an arm64 executable,"
	echo "      so only the .ndll is universal.  That combination is valid: the"
	echo "      universal .ndll is used by both slices of any future universal binary."
	echo "      See README 19.3 for making the executable universal too."
	APP_BIN="$OUT/CodenameEngine-arm64"
else
	lipo -create "$OUT/CodenameEngine-arm64" "$OUT/CodenameEngine-intel-last" \
	     -output "$OUT/CodenameEngine-universal"
	APP_BIN="$OUT/CodenameEngine-universal"
fi

# ------------------------------------------------------------------ install
say "installing into the bundle"
cp "$OUT/lime-universal.ndll" "$APP/Contents/MacOS/lime.ndll"
cp "$APP_BIN" "$BIN"
# Replacing files inside an .app invalidates its signature; without re-signing,
# macOS SIGKILLs the process on launch ("Killed: 9").
codesign --force --deep --sign - "$APP"
codesign -dv "$APP" 2>&1 | head -2

echo
echo "done. verify with:"
echo "  \"$BIN\" | head -1"
echo "  -> [cne-metal] bgfx renderer: Metal (Metal: yes), WxH, ..."
