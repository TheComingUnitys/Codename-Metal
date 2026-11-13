#!/usr/bin/env bash
# Build and run the Metal 4 capability probes (metal-cpp, no bgfx).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/metal/third_party/metal-cpp"
OUT="${TMPDIR:-/tmp}/cne-metal4-probe"
mkdir -p "$OUT"
COMMON=(-std=c++17 -fno-objc-arc -I "$CPP" -I "$ROOT/metal/tools")
FRAMEWORKS=(-framework Metal -framework Foundation -framework QuartzCore -framework AppKit)
buildrenderer() { clang++ "${COMMON[@]}" "$ROOT/metal/tools/$1" "$ROOT/metal/tools/metal4_renderer.mm" "$ROOT/metal/tools/metal4_private.mm" -o "$OUT/$2" "${FRAMEWORKS[@]}"; "$OUT/$2"; }
build() { clang++ "${COMMON[@]}" "$ROOT/metal/tools/$1" "$ROOT/metal/tools/metal4_private.mm" -o "$OUT/$2" "${FRAMEWORKS[@]}"; "$OUT/$2"; }
build metal4_probe.mm probe
case "${1:-}" in
	--present)  build metal4_present.mm present ;;
	--quad)     build metal4_quad.mm quad ;;
	--sprite)   build metal4_sprite.mm sprite ;;
	--scene)    build metal4_scene.mm scene ;;
	--renderer) buildrenderer metal4_renderer_test.mm renderer_test ;;
	--window)   buildrenderer metal4_window.mm window ;;
	--batch)    buildrenderer metal4_batch.mm batch ;;
	--cache)    buildrenderer metal4_cache.mm cache ;;
esac
