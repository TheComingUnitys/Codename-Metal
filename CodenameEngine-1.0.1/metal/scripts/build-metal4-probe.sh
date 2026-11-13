#!/usr/bin/env bash
# Build and run the Metal 4 capability probes (metal-cpp, no bgfx).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/metal/third_party/metal-cpp"
OUT="${TMPDIR:-/tmp}/cne-metal4-probe"
mkdir -p "$OUT"
COMMON=(-std=c++17 -fno-objc-arc -I "$CPP")
FRAMEWORKS=(-framework Metal -framework Foundation -framework QuartzCore -framework AppKit)
build() { clang++ "${COMMON[@]}" "$ROOT/metal/tools/$1" "$ROOT/metal/tools/metal4_private.mm" -o "$OUT/$2" "${FRAMEWORKS[@]}"; }

build metal4_probe.mm probe
"$OUT/probe"
case "${1:-}" in
	--present) build metal4_present.mm present; "$OUT/present" ;;
	--quad)    build metal4_quad.mm quad;       "$OUT/quad" ;;
	--sprite)  build metal4_sprite.mm sprite;   "$OUT/sprite" ;;
	--scene)   build metal4_scene.mm scene;     "$OUT/scene" ;;
esac
