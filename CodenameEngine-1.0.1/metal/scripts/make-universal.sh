#!/usr/bin/env bash
# Build a Universal (arm64 + x86_64) Codename Engine with the Metal renderer,
# and optionally package it as a signed DMG.  This is the pipeline used for the
# GitHub releases (v1.0.1-metal.x).
#
#   ./metal/scripts/make-universal.sh                 # build + install universal app
#   ./metal/scripts/make-universal.sh --dmg           # ... and package a DMG
#   ./metal/scripts/make-universal.sh --check         # preflight only (no build)
#   ./metal/scripts/make-universal.sh --dry-run       # print the plan, change nothing
#   ./metal/scripts/make-universal.sh --verify <app>  # check archs/signature of an app
#
# Environment overrides: PROJECT_DIR, LIME_DIR, HXCPP_DIR, BGFX_DIR, OUT, REUSE=1.
#
# Why the x86_64 slice needs its own pass: Lime asks the *host* for the target
# architecture, so 'lime build mac' always emits arm64 here.  The Intel slice is
# produced by invoking haxe/hxcpp with HXCPP_M64 directly (hxcpp maps it to
# HXCPP_ARCH=x86_64 in mac-toolchain.xml and writes to obj/darwin64, leaving the
# arm64 objects untouched).  The same trick, via -DCNE_X64, builds the Intel ndll.
#
# Host prerequisites (see README 5.x): haxe + haxelib, a Lime dev checkout,
# hxcpp, and bgfx/bx/bimg built for *both* osx-arm64 and osx-x64.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
METAL_DIR="$(dirname "$HERE")"
PROJECT_DIR="${PROJECT_DIR:-$(cd "$METAL_DIR/.." && pwd)}"
REPO_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
BGFX_DIR="${BGFX_DIR:-$REPO_DIR/bgfx-master}"
LIME_DIR="${LIME_DIR:-$HOME/haxelib-dev/lime-832}"
HXCPP="${HXCPP_DIR:-$HOME/haxelib/hxcpp/4,3,2}"
OUT="${OUT:-/tmp/cne-universal}"

APP="$PROJECT_DIR/export/release/macos/bin/CodenameEngine.app"
BIN="$APP/Contents/MacOS/CodenameEngine"
NDLL="$LIME_DIR/ndll/MacArm64/lime-64.ndll"
BUILD_XML="$LIME_DIR/project/Build.xml"
ARM_BIN="$OUT/CodenameEngine-arm64"
X64_BIN="$OUT/CodenameEngine-x86_64"
ARM_NDLL="$OUT/lime-64-arm64.ndll"
X64_NDLL="$OUT/lime-64-x86_64.ndll"
UNI_BIN="$OUT/CodenameEngine-universal"
UNI_NDLL="$OUT/lime-universal.ndll"

DO_BUILD=1
DO_DMG=0
DO_VERIFY=""
KEEP_MODS=0
NO_SIGN=0
DMG_PATH=""
VOLNAME="Codename Engine"

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
info() { printf '   %s\n' "$1"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$1" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$1" >&2; exit 1; }
run() {
	if [ "${DRY_RUN:-0}" = "1" ]; then
		printf '   [dry-run] %s\n' "$*"
	else
		eval "$@"
	fi
}

usage() {
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
	case "$1" in
		--dmg) DO_DMG=1; shift
			if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then DMG_PATH="$1"; shift; fi ;;
		--keep-mods) KEEP_MODS=1; shift ;;
		--no-sign) NO_SIGN=1; shift ;;
		--check) DO_BUILD=0; DO_DMG=0; shift ;;
		--dry-run) DRY_RUN=1; shift ;;
		--verify) DO_BUILD=0; DO_DMG=0; DO_VERIFY="$2"; shift 2 ;;
		--reuse) REUSE=1; shift ;;
		--out) OUT="$2"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) die "unknown argument: $1 (try --help)" ;;
	esac
done


# ------------------------------------------------------------------ preflight
say "preflight"
[ -d "$PROJECT_DIR/assets" ] || die "$PROJECT_DIR is not the Codename Engine project"
[ -f "$BUILD_XML" ] || die "no Lime checkout at $LIME_DIR (project/Build.xml missing)"
[ -f "$HXCPP/toolchain/mac-toolchain.xml" ] || die "no hxcpp at $HXCPP"
command -v haxe >/dev/null || die "haxe not on PATH"
command -v haxelib >/dev/null || die "haxelib not on PATH"
[ -d "$BGFX_DIR/.build/osx-arm64/bin" ] || die "missing arm64 bgfx archives in $BGFX_DIR/.build/osx-arm64/bin"
[ -d "$BGFX_DIR/.build/osx-x64/bin" ] || die "missing Intel bgfx archives in $BGFX_DIR/.build/osx-x64/bin"
info "project   : $PROJECT_DIR"
info "lime      : $LIME_DIR"
info "hxcpp     : $HXCPP"
info "bgfx      : $BGFX_DIR"
info "slices    : $OUT"
[ -d "$BGFX_DIR" ] || warn "BGFX_DIR does not exist"

if [ -n "$DO_VERIFY" ]; then
	say "verify $DO_VERIFY"
	[ -d "$DO_VERIFY" ] || die "no such app: $DO_VERIFY"
	VBIN="$DO_VERIFY/Contents/MacOS/CodenameEngine"
	VNDLL="$DO_VERIFY/Contents/MacOS/lime.ndll"
	[ -f "$VBIN" ] || die "missing executable in $DO_VERIFY"
	lipo -archs "$VBIN" | tee "$OUT/verify.txt"
	lipo -archs "$VNDLL" | tee -a "$OUT/verify.txt"
	grep -q 'arm64' "$OUT/verify.txt" || die "app executable is not arm64"
	grep -q 'x86_64' "$OUT/verify.txt" || die "app executable is not x86_64"
	codesign --verify --deep --strict "$DO_VERIFY" && info "codesign OK"
	exit 0
fi

if [ "$DO_BUILD" = "0" ]; then
	say "check only (no build requested)"
	[ "${DRY_RUN:-0}" = "1" ] && info "dry-run: would build arm64 pass, x86_64 pass, lipo, install, sign$([ "$DO_DMG" = "1" ] && echo ', package DMG')"
	info "preflight passed"
	exit 0
fi

mkdir -p "$OUT"
[ "${REUSE:-0}" = "1" ] || rm -f "$ARM_BIN" "$X64_BIN" "$ARM_NDLL" "$X64_NDLL" "$UNI_BIN" "$UNI_NDLL"

restore_lime() {
	[ "${DRY_RUN:-0}" = "1" ] && return 0
	sed -i "" 's|\.build/osx-x64/bin|.build/osx-arm64/bin|g' "$BUILD_XML" 2>/dev/null || true
}
trap restore_lime EXIT

# ------------------------------------------------------------------ arm64 pass
if [ "${REUSE:-0}" = "1" ] && [ -f "$ARM_BIN" ] && [ -f "$ARM_NDLL" ]; then
	say "pass 1/3: arm64 (reusing $OUT)"
else
	say "pass 1/3: arm64"
	restore_lime
	run "rm -rf '$LIME_DIR/project/obj'"
	run "( cd '$LIME_DIR' && haxelib run lime rebuild mac -Dmetal )"
	[ "${DRY_RUN:-0}" = "1" ] || [ -f "$NDLL" ] || die "arm64 rebuild produced no $NDLL"
	run "( cd '$PROJECT_DIR' && haxelib run lime build mac )"
	[ "${DRY_RUN:-0}" = "1" ] || [ -f "$BIN" ] || die "lime build produced no $BIN"
	run "cp '$NDLL' '$ARM_NDLL'"
	run "cp '$BIN' '$ARM_BIN'"
fi

# ------------------------------------------------------------------ x86_64 pass
if [ "${REUSE:-0}" = "1" ] && [ -f "$X64_BIN" ] && [ -f "$X64_NDLL" ]; then
	say "pass 2/3: x86_64 (reusing $OUT)"
else
	say "pass 2/3: x86_64"
	run "rm -rf '$LIME_DIR/project/obj'"
	run "sed -i '' 's|\\.build/osx-arm64/bin|.build/osx-x64/bin|g' '$BUILD_XML'"
	run "( cd '$LIME_DIR' && haxelib run lime rebuild mac -Dmetal -DCNE_X64 )"
	[ "${DRY_RUN:-0}" = "1" ] || [ -f "$NDLL" ] || die "Intel rebuild produced no $NDLL"
	run "cp '$NDLL' '$X64_NDLL'"
	run "( cd '$PROJECT_DIR' && haxe export/release/macos/haxe/release.hxml -D HXCPP_CLANG -D HXCPP_M64 )"
	run "( cd '$PROJECT_DIR/export/release/macos/obj' && haxelib run hxcpp Build.xml -DHXCPP_CLANG -DHXCPP_M64 )"
	[ "${DRY_RUN:-0}" = "1" ] || [ -f "$PROJECT_DIR/export/release/macos/obj/ApplicationMain" ] || die "Intel hxcpp link produced no ApplicationMain"
	run "cp '$PROJECT_DIR/export/release/macos/obj/ApplicationMain' '$X64_BIN'"
	restore_lime
fi

if [ "${DRY_RUN:-0}" = "1" ]; then
	say "dry-run: plan"
	info "would lipo $ARM_BIN + $X64_BIN -> $UNI_BIN"
	info "would lipo $ARM_NDLL + $X64_NDLL -> $UNI_NDLL"
	info "would install into $APP and re-sign"
	if [ "$DO_DMG" = "1" ]; then info "would package DMG (default under ~/Downloads)"; fi
	exit 0
fi

# ------------------------------------------------------------------ merge + install
say "pass 3/3: lipo + install"
for slice in "$ARM_BIN" "$X64_BIN" "$ARM_NDLL" "$X64_NDLL"; do
	info "$(basename "$slice"): $(lipo -archs "$slice")"
done
lipo -create "$ARM_BIN" "$X64_BIN" -output "$UNI_BIN"
lipo -create "$ARM_NDLL" "$X64_NDLL" -output "$UNI_NDLL"
info "exe : $(lipo -archs "$UNI_BIN") ($(stat -f%z "$UNI_BIN") bytes)"
info "ndll: $(lipo -archs "$UNI_NDLL") ($(stat -f%z "$UNI_NDLL") bytes)"

[ -d "$APP" ] || die "no app bundle at $APP - run 'haxelib run lime build mac' first"
cp "$UNI_BIN" "$BIN"
cp "$UNI_NDLL" "$APP/Contents/MacOS/lime.ndll"
if [ "$NO_SIGN" = "0" ]; then
	codesign --force --deep --sign - "$APP"
	codesign --verify --deep --strict "$APP" && info "codesign OK"
fi

# ------------------------------------------------------------------ DMG
if [ "$DO_DMG" = "1" ]; then
	[ -n "$DMG_PATH" ] || DMG_PATH="$HOME/Downloads/CodenameEngine-Metal-universal.dmg"
	say "packaging DMG -> $DMG_PATH"
	WORK="$(mktemp -d "${TMPDIR:-/tmp}/cne-dmg.XXXXXX")"
	cp -R "$APP" "$WORK/"
	STAGED="$WORK/$(basename "$APP")"
	if [ "$KEEP_MODS" = "0" ]; then
		rm -rf "$STAGED/Contents/Resources/mods"
		mkdir -p "$STAGED/Contents/Resources/mods"
		printf '(install mods here, e.g. dustin-mod-master)\\n' > "$STAGED/Contents/Resources/mods/readme.txt"
	fi
	if [ "$NO_SIGN" = "0" ]; then
		codesign --force --deep --sign - "$STAGED"
		codesign --verify --deep --strict "$STAGED" && info "staged codesign OK"
	fi
	ln -s /Applications "$WORK/Applications"
	rm -f "$DMG_PATH"
	hdiutil create -volname "$VOLNAME" -srcfolder "$WORK" -ov -format UDZO \
		-imagekey zlib-level=9 "$DMG_PATH" >/dev/null
	rm -rf "$WORK"
	hdiutil verify "$DMG_PATH" | tail -1
	info "sha256: $(shasum -a 256 "$DMG_PATH" | awk '{print $1}')"
	info "size  : $(stat -f%z "$DMG_PATH") bytes"
	info "path  : $DMG_PATH"
fi

say "done"
info "executable: $(lipo -archs "$BIN")"
info "lime.ndll : $(lipo -archs "$APP/Contents/MacOS/lime.ndll")"
info "next: ./metal/scripts/make-universal.sh --verify '$APP'"
