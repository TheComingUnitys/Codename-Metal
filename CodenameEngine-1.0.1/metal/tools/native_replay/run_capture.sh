#!/bin/bash
# Runs the game with the frame recorder + raw screenshot for the native-Metal A/B
# prototype.  Usage: run_capture.sh <frame> <seconds> [autokeys]
set -u
FRAME="${1:-360}"
SECS="${2:-40}"
KEYS="${3:-}"
OUT=/tmp/cne-capture/f${FRAME}
rm -rf "$OUT" /tmp/cne-shot
mkdir -p /tmp/cne-shot
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1/export/release/macos/bin || exit 1
while ! mkdir /tmp/cne-game.lock 2>/dev/null; do echo waiting_for_game_lock; sleep 5; done
trap 'rmdir /tmp/cne-game.lock 2>/dev/null' EXIT
if pgrep -f 'MacOS/CodenameEngine$' >/dev/null; then echo "another instance is running, aborting"; exit 1; fi
export CNE_METAL_CAPTURE_FRAME="$FRAME"
export CNE_METAL_SHOT_RAW="$FRAME:/tmp/cne-shot/frame"
export CNE_METAL_SKIP_PROGRAM="4,8"
[ -n "$KEYS" ] && export CNE_METAL_AUTOKEYS="$KEYS"
nohup ./CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-capture-game.log 2>&1 &
L=$!; sleep 1; C=$(pgrep -P $L | head -1); C=${C:-$L}
echo "launcher=$L app=$C"
for i in $(seq 1 $SECS); do
  kill -0 "$C" 2>/dev/null || { echo "app exited at ${i}s"; break; }
  [ -f /tmp/cne-shot/frame.raw ] && { echo "got screenshot at ${i}s"; break; }
  sleep 1
done
kill -TERM "$C" "$L" 2>/dev/null; sleep 2; kill -9 "$C" "$L" 2>/dev/null
echo "capture dir:"; ls -la "$OUT" 2>/dev/null | head -8
echo "draw files: $(ls "$OUT"/d*.txt 2>/dev/null | wc -l)"
echo "shot: $(ls -la /tmp/cne-shot/ 2>/dev/null | head -4)"
