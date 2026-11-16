#!/usr/bin/env bash
# Build and run the headless Metal 4 app-renderer self-test.
#
# This does NOT launch the game: it drives metal/native/Metal4App.mm directly
# (offscreen render target + CPU readback) so the attribute-slot, constant
# attribute, per-stage uniform and resource-lifetime paths can be verified
# before asking a human to play-test.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE="$(cd "$HERE/../native" && pwd)"
OUT="${TMPDIR:-/tmp}/cne-metal4-selftest"

SDK=""
for candidate in $(ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk 2>/dev/null | sort -V -r); do
	if [ -f "$candidate/System/Library/Frameworks/Metal.framework/Versions/A/Headers/MTL4ArgumentTable.h" ]; then
		SDK="$candidate"
		break
	fi
done
if [ -z "$SDK" ]; then
	echo "error: no SDK with Metal 4 headers (MTL4ArgumentTable.h) found" >&2
	exit 2
fi

echo "sdk: $SDK"
clang++ -x objective-c++ -std=c++17 -fobjc-arc -O0 -g \
	-isysroot "$SDK" -mmacosx-version-min=26.0 \
	-I"$NATIVE" \
	-framework Metal -framework QuartzCore -framework Foundation \
	-o "$OUT" \
	"$HERE/../autotests/metal4_selftest.mm" "$NATIVE/Metal4App.mm"

echo "built: $OUT"
MTL_DEBUG_LAYER="${MTL_DEBUG_LAYER:-1}" "$OUT"
