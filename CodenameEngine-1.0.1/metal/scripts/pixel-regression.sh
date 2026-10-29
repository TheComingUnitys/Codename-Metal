#!/usr/bin/env bash
# pixel-regression.sh - capture CNE_METAL_SHOT_RAW frames and compare them with goldens.
#
#   ./pixel-regression.sh --selftest
#   ./pixel-regression.sh --app <CodenameEngine.app> --scenario menu
#   ./pixel-regression.sh --app ... --scenario menu --update     # (re)write goldens
#
# Scenario files live in metal/autotests/scenarios/<name>.json:
#   {
#     "autokeys": "120:40",                       # optional CNE_METAL_AUTOKEYS
#     "captures": [{"frame": 240, "name": "stage"}],
#     "tolerance": {"diffPixels": 4000, "mean": 2.0, "abs": 24}
#   }
# Goldens: metal/autotests/golden/<scenario>/<name>.{raw,txt} (machine/driver
# specific - regenerate with --update on the reference machine).
#
# Exit codes: 0 all captures match, 1 regression, 2 usage error.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
METAL="$ROOT/CodenameEngine-1.0.1/metal"
SCENARIO_DIR="$METAL/autotests/scenarios"
GOLDEN_DIR="$METAL/autotests/golden"
DIFF_TOOL="$METAL/tools/frame_diff.py"

APP=""
SCENARIO=""
UPDATE=0
OUTDIR=""
TIMEOUT=90

usage() {
	sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
	case "$arg" in
		--selftest) exec python3 "$DIFF_TOOL" --selftest ;;
		--update) UPDATE=1 ;;
		--app) shift; APP="$1" ;;
		--scenario) shift; SCENARIO="$1" ;;
		--outdir) shift; OUTDIR="$1" ;;
		--timeout) shift; TIMEOUT="$1" ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown argument: $arg" >&2; exit 2 ;;
	esac
done

if [ -z "$APP" ] || [ -z "$SCENARIO" ]; then
	echo "error: --app and --scenario are required (or use --selftest)" >&2
	exit 2
fi

SCENARIO_FILE="$SCENARIO_DIR/$SCENARIO.json"
[ -f "$SCENARIO_FILE" ] || { echo "error: no such scenario: $SCENARIO_FILE" >&2; exit 2; }

[ -z "$OUTDIR" ] && OUTDIR="$(mktemp -d "${TMPDIR:-/tmp}/cne-pixel-XXXXXX")"
mkdir -p "$OUTDIR"

AUTOKEYS="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("autokeys",""))' "$SCENARIO_FILE")"
SHOT_SPEC="$(python3 -c 'import json,sys,os; cfg=json.load(open(sys.argv[1])); out=sys.argv[2]; print(",".join("%d:%s" % (c["frame"], os.path.join(out, c["name"])) for c in cfg["captures"]))' "$SCENARIO_FILE" "$OUTDIR")"

echo "[pixel-regression] scenario=$SCENARIO captures=$SHOT_SPEC"
echo "[pixel-regression] outdir=$OUTDIR"

LOG="$OUTDIR/engine.log"
env CNE_METAL_SHOT_RAW="$SHOT_SPEC" ${AUTOKEYS:+CNE_METAL_AUTOKEYS="$AUTOKEYS"} \
	"$APP/Contents/MacOS/CodenameEngine" >"$LOG" 2>&1 &
PID=$!

# Wait until every capture has been written, then stop the engine.
EXPECTED=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["captures"]))' "$SCENARIO_FILE")
for _ in $(seq 1 "$TIMEOUT"); do
	COUNT=$(ls "$OUTDIR"/*.raw 2>/dev/null | wc -l | tr -d ' ')
	[ "$COUNT" -ge "$EXPECTED" ] && break
	sleep 1
done
sleep 1
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

COUNT=$(ls "$OUTDIR"/*.raw 2>/dev/null | wc -l | tr -d ' ')
if [ "$COUNT" -lt "$EXPECTED" ]; then
	echo "error: captured $COUNT/$EXPECTED frames (see $LOG)" >&2
	exit 1
fi

TOLERANCE_JSON="$(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1])).get("tolerance",{})))' "$SCENARIO_FILE")"
TOL_ARGS="$(python3 -c 'import json,sys; t=json.loads(sys.argv[1]); a=[]
if "diffPixels" in t: a += ["--max-diff-pixels", str(t["diffPixels"])]
if "mean" in t: a += ["--max-mean", str(t["mean"])]
if "abs" in t: a += ["--max-abs", str(t["abs"])]
print(" ".join(a))' "$TOLERANCE_JSON")"

STATUS=0
while IFS= read -r NAME; do
	ACTUAL="$OUTDIR/$NAME"
	GOLDEN="$GOLDEN_DIR/$SCENARIO/$NAME"
	if [ "$UPDATE" = "1" ]; then
		mkdir -p "$GOLDEN_DIR/$SCENARIO"
		cp "$ACTUAL.raw" "$GOLDEN.raw"
		cp "$ACTUAL.txt" "$GOLDEN.txt"
		echo "[pixel-regression] updated golden: $GOLDEN.raw"
		continue
	fi
	if [ ! -f "$GOLDEN.raw" ]; then
		echo "error: missing golden $GOLDEN.raw (run with --update first)" >&2
		STATUS=1
		continue
	fi
	# shellcheck disable=SC2086
	if ! python3 "$DIFF_TOOL" "$GOLDEN" "$ACTUAL" $TOL_ARGS --diff-image "$OUTDIR/$NAME.diff.bmp"; then
		STATUS=1
	fi
done < <(python3 -c 'import json,sys; [print(c["name"]) for c in json.load(open(sys.argv[1]))["captures"]]' "$SCENARIO_FILE")

if [ "$STATUS" = "0" ]; then
	echo "[pixel-regression] PASS ($EXPECTED capture(s), scenario $SCENARIO)"
fi
exit "$STATUS"
