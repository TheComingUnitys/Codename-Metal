#!/usr/bin/env bash
# verify.sh - one-shot verification for the Metal backend (no app build required).
#
#   ./verify.sh                 translator tests + AOT strict audit + docs consistency
#   ./verify.sh --no-aot        skip the AOT cache audit (no haxelib deps needed)
#   ./verify.sh --with-native   also run the C++/bgfx compile check
#
# Exit code 0 means every enabled check passed.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$ROOT/CodenameEngine-1.0.1"
LIME="${LIME:-$HOME/haxelib-dev/lime-832}"
HXCPP="${HXCPP:-$HOME/haxelib/hxcpp/4,3,2}"

RUN_AOT=1
RUN_NATIVE=0

for arg in "$@"; do
	case "$arg" in
		--no-aot) RUN_AOT=0 ;;
		--with-native) RUN_NATIVE=1 ;;
		-h|--help)
			sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*)
			echo "unknown argument: $arg" >&2
			exit 2
			;;
	esac
done

HAXE="${HAXE:-haxe}"
step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

step "1/4 translator unit tests"
"$HAXE" -cp "$PROJECT/metal/haxe" -main lime.graphics.metal.TestTranslate --interp
"$HAXE" -cp "$PROJECT/metal/haxe" -main lime.graphics.metal.TestTranslateCompat --interp

step "2/4 screenshot diff self test"
python3 "$PROJECT/metal/tools/frame_diff.py" --selftest

if [ "$RUN_AOT" = "1" ]; then
	step "3/4 AOT cache staleness + strict coverage audit"
	# --check: the committed ShaderAotCache.hx must match a fresh generation.
	# --strict: .metallib coverage may only regress against metal/audit/aot-expected.json.
	python3 "$PROJECT/metal/scripts/gen_shader_aot.py" --check --strict
else
	step "3/4 AOT audit skipped (--no-aot)"
fi

step "4/4 documentation consistency"
python3 "$PROJECT/metal/scripts/check-docs.py"

if [ "$RUN_NATIVE" = "1" ]; then
	step "extra: native backend compile check"
	"$PROJECT/metal/scripts/check-compile.sh" --lime "$LIME" --hxcpp "$HXCPP" --out "${TMPDIR:-/tmp}/cne-verify-obj"
fi

printf '\n\033[1;32mverify.sh: all enabled checks passed\033[0m\n'
