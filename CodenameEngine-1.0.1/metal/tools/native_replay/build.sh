#!/bin/bash
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-/tmp/native_replay}"
clang++ -std=c++17 -O2 -fobjc-arc -x objective-c++ \
  -framework Foundation -framework Metal -framework QuartzCore \
  "$HERE/main.mm" -o "$OUT"
echo "built $OUT"
