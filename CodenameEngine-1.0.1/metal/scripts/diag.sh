#!/usr/bin/env bash
# diag.sh - unified facade for the Metal backend diagnostics.
#
# Legacy CNE_METAL_* environment variables keep working; this script only maps
# stable category names onto them (and documents the mapping with --list).
#
#   ./metal/scripts/diag.sh --list
#   ./metal/scripts/diag.sh --print stats,fbo
#   ./metal/scripts/diag.sh --categories profile,hitch,draws -- <app/Contents/MacOS/CodenameEngine>
#   ./metal/scripts/diag.sh --categories stats --shot 240:/tmp/stage -- <app binary>
#   ./metal/scripts/diag.sh --capture-frame 240 --capture-dir /tmp/capture -- <app binary>
#
# Exit codes: 0 ok, 2 usage error.

set -euo pipefail

CATEGORIES="stats profile hitch hitch-detail draws fbo uv tex coverage shader aot mtl debug"

usage() {
	sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

describe() {
	case "$1" in
		stats)         echo "frame counters: present/frame, bgfxDraws, glDrawElements, submitDraw, backbuffer" ;;
		profile)       echo "CPU front-end cost per 240 frames (draws/bind/submit/uniform/texture)" ;;
		hitch)         echo "report frames > 34 ms" ;;
		hitch-detail)  echo "hitch split into game/submit/present/rest" ;;
		draws)         echo "per-draw probe: program/fbo/tex/size/rt/m0/m5/m13" ;;
		fbo)           echo "framebuffer create/invalidate log" ;;
		uv)            echo "screen composite flip decision log" ;;
		tex)           echo "texture create log (requested size vs screen size)" ;;
		coverage)      echo "write the lime_gl_* primitive coverage report" ;;
		shader)        echo "GLSL/GLSL->MSL shader audit log" ;;
		aot)           echo "AOT cache lookup summary (hits/misses/translations)" ;;
		mtl)           echo "Metal shader/program load + compile summary" ;;
		debug)         echo "general backend debug output" ;;
		*) return 1 ;;
	esac
}

env_for() {
	case "$1" in
		stats)        echo "CNE_METAL_STATS=1" ;;
		profile)      echo "CNE_METAL_PROFILE=1" ;;
		hitch)        echo "CNE_METAL_HITCH=1" ;;
		hitch-detail) echo "CNE_METAL_HITCH_DETAIL=1" ;;
		draws)        echo "CNE_METAL_DRAWPROBE=1" ;;
		fbo)          echo "CNE_METAL_FBOLOG=1" ;;
		uv)           echo "CNE_METAL_UVLOG=1" ;;
		tex)          echo "CNE_METAL_TEXLOG=1" ;;
		coverage)     echo "CNE_METAL_COVERAGE=1" ;;
		shader)       echo "CNE_METAL_SHADER_AUDIT=1" ;;
		aot)          echo "CNE_METAL_AOT_AUDIT=1" ;;
		mtl)          echo "CNE_METAL_MTL_AUDIT=1" ;;
		debug)        echo "CNE_METAL_DEBUG=1" ;;
		*) return 1 ;;
	esac
}

LIST=0
PRINT=0
CATS=""
CAPTURE_FRAME=""
CAPTURE_DIR=""
SHOTS=""
KEYS=""
CMD=()

while [ $# -gt 0 ]; do
	case "$1" in
		--list) LIST=1; shift ;;
		--print) PRINT=1; shift
			if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then CATS="$1"; shift; fi ;;
		--categories|-c) CATS="$2"; shift 2 ;;
		--shot) SHOTS="${SHOTS:+$SHOTS,}$2"; shift 2 ;;
		--capture-frame) CAPTURE_FRAME="$2"; shift 2 ;;
		--capture-dir) CAPTURE_DIR="$2"; shift 2 ;;
		--keys) KEYS="$2"; shift 2 ;;
		--) shift; CMD=("$@"); break ;;
		-h|--help) usage; exit 0 ;;
		*) echo "error: unknown argument: $1 (try --help)" >&2; exit 2 ;;
	esac
done

if [ "$LIST" = "1" ]; then
	printf '%-14s %s\n' "category" "what it enables (legacy env var in parentheses)"
	for c in $CATEGORIES; do
		printf '%-14s %s\n' "$c" "$(describe "$c") ($(env_for "$c"))"
	done
	printf '\n%-14s %s\n' "(special)" "--shot <frame>:<path> -> CNE_METAL_SHOT_RAW, --keys -> CNE_METAL_AUTOKEYS"
	printf '%-14s %s\n' "(special)" "--capture-frame N [--capture-dir DIR] -> CNE_METAL_CAPTURE_FRAME/DIR"
	exit 0
fi

ENVVARS=()
[ -n "$CATS" ] && for c in $(echo "$CATS" | tr ',' ' '); do
	if [ "$c" = "all" ]; then
		for x in $CATEGORIES; do ENVVARS+=("$(env_for "$x")"); done
		continue
	fi
	v="$(env_for "$c")" || { echo "error: unknown category: $c (see --list)" >&2; exit 2; }
	ENVVARS+=("$v")
done
[ -n "$SHOTS" ] && ENVVARS+=("CNE_METAL_SHOT_RAW=$SHOTS")
[ -n "$KEYS" ] && ENVVARS+=("CNE_METAL_AUTOKEYS=$KEYS")
[ -n "$CAPTURE_FRAME" ] && ENVVARS+=("CNE_METAL_CAPTURE_FRAME=$CAPTURE_FRAME")
[ -n "$CAPTURE_DIR" ] && ENVVARS+=("CNE_METAL_CAPTURE_DIR=$CAPTURE_DIR")

if [ "$PRINT" = "1" ]; then
	for v in "${ENVVARS[@]:-}"; do
		[ -n "$v" ] && printf 'export %s\n' "$v"
	done
	exit 0
fi

if [ ${#CMD[@]} -eq 0 ]; then
	echo "error: no command given (use -- <app binary>, or --print/--list)" >&2
	usage >&2
	exit 2
fi

exec env "${ENVVARS[@]:-}" "${CMD[@]}"
