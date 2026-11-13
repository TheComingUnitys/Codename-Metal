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

if [ "${1:-}" = "--present" ]; then build metal4_present.mm present; "$OUT/present"; fi
if [ "${1:-}" = "--quad" ]; then build metal4_quad.mm quad; "$OUT/quad"; fi
if [ "${1:-}" = "--sprite" ]; then build metal4_sprite.mm sprite; "$OUT/sprite"; fi
