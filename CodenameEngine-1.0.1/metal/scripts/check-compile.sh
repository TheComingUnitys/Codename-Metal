#!/usr/bin/env bash
# Compile-check the Metal rendering backend against the real Lime + bgfx + hxcpp headers.
#
#   ./metal/scripts/check-compile.sh [--lime <path>] [--bgfx <path>]
#
# This is the fastest feedback loop for the native backend: it compiles
# MetalContext.cpp and MetalBindings.cpp to object files without building Lime or
# the game, so C++/API errors surface in seconds instead of after a 20 minute build.
#
# It uses exactly the same defines the real Metal build uses
# (`-DLIME_METAL`, bgfx v166 headers, Lime's `project/include`).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
METAL_DIR="$(dirname "$HERE")"
PROJECT_DIR="$(cd "$METAL_DIR/.." && pwd)"
WORKSPACE="$(cd "$PROJECT_DIR/.." && pwd)"

LIME_DIR="${LIME_DIR:-}"
pick_dir() {  # pick_dir <env-value> <candidate...>
	local first="$1"; shift
	if [ -n "$first" ]; then echo "$first"; return; fi
	for c in "$@"; do [ -d "$c" ] && { echo "$c"; return; }; done
	echo ""
}
BGFX_DIR="$(pick_dir "${BGFX_DIR:-}" \
	"$WORKSPACE/bgfx-master" \
	"$PROJECT_DIR/../bgfx-master" \
	"$HOME/Codename-Metal/bgfx-master" \
	"$HOME/bgfx-master")"
HXCPP_DIR="$(pick_dir "${HXCPP_DIR:-}" \
	"$PROJECT_DIR/.haxelib/hxcpp/4,3,2" \
	"$HOME/haxelib/hxcpp/4,3,2" \
	"$HOME/haxelib/hxcpp/4,3,3")"
OUT_DIR="${OUT_DIR:-/tmp/cne-metal-obj}"

while [ $# -gt 0 ]; do
	case "$1" in
		--lime) LIME_DIR="$2"; shift 2 ;;
		--bgfx) BGFX_DIR="$2"; shift 2 ;;
		--hxcpp) HXCPP_DIR="$2"; shift 2 ;;
		--out) OUT_DIR="$2"; shift 2 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

if [ -z "$LIME_DIR" ]; then
	for candidate in "$PROJECT_DIR/dev-libs/lime" "$HOME/haxelib-dev/lime" "$PROJECT_DIR/.haxelib/lime/8,3,2"; do
		if [ -f "$candidate/project/src/graphics/opengl/OpenGLBindings.cpp" ]; then
			LIME_DIR="$candidate"; break
		fi
	done
fi

for pair in "lime:$LIME_DIR" "bgfx:$BGFX_DIR" "hxcpp:$HXCPP_DIR"; do
	name="${pair%%:*}"; path="${pair#*:}"
	if [ -z "$path" ] || [ ! -d "$path" ]; then
		echo "error: $name directory not found (got '$path')" >&2
		exit 2
	fi
done

mkdir -p "$OUT_DIR"

INC=(
	"-I$METAL_DIR/native"
	"-I$LIME_DIR/project/include"
	"-I$LIME_DIR/project/src/graphics/opengl"
	"-I$LIME_DIR/project/lib/sdl/include"
	"-I$LIME_DIR/project/lib/hashlink/src"
	"-I$BGFX_DIR/include"
	"-I$BGFX_DIR/bx/include"
	"-I$BGFX_DIR/3rdparty"
	"-I$HXCPP_DIR/include"
)

DEFS=(-DLIME_METAL -DLIME_SDL -DHX_MACOS)

echo "lime   : $LIME_DIR"
echo "bgfx   : $BGFX_DIR"
echo "hxcpp  : $HXCPP_DIR"
echo

fail=0
for src in MetalContext MetalBindings; do
	printf '  compiling %-18s ... ' "$src.cpp"
	if clang++ -c -std=gnu++17 -O1 "${DEFS[@]}" "${INC[@]}" \
		-o "$OUT_DIR/$src.o" "$METAL_DIR/native/$src.cpp" 2> "$OUT_DIR/$src.err"; then
		echo "OK  ($(du -h "$OUT_DIR/$src.o" | cut -f1))"
	else
		echo "FAILED"
		sed -n '1,40p' "$OUT_DIR/$src.err" | sed 's/^/      /'
		fail=1
	fi
done

if [ "$fail" -eq 0 ]; then
	echo
	echo "primitive symbols in MetalBindings.o:"
	nm -gU "$OUT_DIR/MetalBindings.o" 2>/dev/null | grep -c "lime_gl_" | sed 's/^/  lime_gl_*: /'
	nm -gU "$OUT_DIR/MetalBindings.o" 2>/dev/null | grep -c "lime_metal_" | sed 's/^/  lime_metal_*: /'
	echo
	BGFX_VER=$(sed -n 's/.*UINT32_C(\([0-9]*\)).*/\1/p' "$BGFX_DIR/include/bgfx/defines.h" | head -1)
	echo "OK - Metal backend compiles against bgfx API v$BGFX_VER and Lime"
fi

exit $fail
