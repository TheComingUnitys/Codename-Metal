#!/usr/bin/env bash
# Build and run the Metal 4 capability probes (metal-cpp, no bgfx).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/metal/third_party/metal-cpp"
OUT="${TMPDIR:-/tmp}/cne-metal4-probe"
mkdir -p "$OUT"
COMMON=(-std=c++17 -fno-objc-arc -I "$CPP" -I "$ROOT/metal/tools")
FRAMEWORKS=(-framework Metal -framework Foundation -framework QuartzCore -framework AppKit)
br() { clang++ "${COMMON[@]}" "$ROOT/metal/tools/$1" "$ROOT/metal/tools/metal4_renderer.mm" "$ROOT/metal/tools/metal4_private.mm" -o "$OUT/$2" "${FRAMEWORKS[@]}"; "$OUT/$2"; }
b() { clang++ "${COMMON[@]}" "$ROOT/metal/tools/$1" "$ROOT/metal/tools/metal4_private.mm" -o "$OUT/$2" "${FRAMEWORKS[@]}"; "$OUT/$2"; }
b metal4_probe.mm probe
case "${1:-}" in
	--present) b metal4_present.mm present ;;
	--quad) b metal4_quad.mm quad ;;
	--sprite) b metal4_sprite.mm sprite ;;
	--scene) b metal4_scene.mm scene ;;
	--renderer) br metal4_renderer_test.mm renderer_test ;;
	--window) br metal4_window.mm window ;;
	--batch) br metal4_batch.mm batch ;;
	--cache) br metal4_cache.mm cache ;;
	--filter) br metal4_filter.mm filter ;;
	--scissor) br metal4_scissor.mm scissor ;;
	--metallib) b metal4_metallib.mm metallib ;;
esac
