#!/usr/bin/env bash
# Build and run the Metal 4 capability probe (metal-cpp, no bgfx).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/metal/third_party/metal-cpp"
OUT="${TMPDIR:-/tmp}/cne-metal4-probe"
mkdir -p "$OUT"
clang++ -std=c++17 -fno-objc-arc -I "$CPP" \
	"$ROOT/metal/tools/metal4_probe.mm" \
	"$ROOT/metal/tools/metal4_private.mm" \
	-o "$OUT/probe" \
	-framework Metal -framework Foundation -framework QuartzCore
"$OUT/probe"
