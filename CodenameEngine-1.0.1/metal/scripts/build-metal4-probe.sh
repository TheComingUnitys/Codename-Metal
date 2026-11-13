#!/usr/bin/env bash
# Build and run the Metal 4 capability probes (metal-cpp, no bgfx).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/metal/third_party/metal-cpp"
OUT="${TMPDIR:-/tmp}/cne-metal4-probe"
mkdir -p "$OUT"

COMMON=(-std=c++17 -fno-objc-arc -I "$CPP")
FRAMEWORKS=(-framework Metal -framework Foundation -framework QuartzCore -framework AppKit)

clang++ "${COMMON[@]}" \
	"$ROOT/metal/tools/metal4_probe.mm" \
	"$ROOT/metal/tools/metal4_private.mm" \
	-o "$OUT/probe" "${FRAMEWORKS[@]}"

"$OUT/probe"

if [ "${1:-}" = "--present" ]; then
	clang++ "${COMMON[@]}" \
		"$ROOT/metal/tools/metal4_present.mm" \
		"$ROOT/metal/tools/metal4_private.mm" \
		-o "$OUT/present" "${FRAMEWORKS[@]}"
	"$OUT/present"
fi
