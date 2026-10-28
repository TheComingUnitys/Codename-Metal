#!/usr/bin/env bash
# One-shot build for the Metal backend on macOS.
#
#   ./build.sh                # full build (native backend + game + deploy)
#   ./build.sh --check        # compile-check only (fast, no app build)
#   ./build.sh --bgfx-only    # (re)build the bgfx static libraries only
#
# Environment:
#   LIME_DIR   path to the patched Lime 8.3.2 checkout (default ~/haxelib-dev/lime-832)
#   HXCPP_DIR  path to hxcpp (default ~/haxelib/hxcpp/4,3,2)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$HERE/CodenameEngine-1.0.1"
LIME_DIR="${LIME_DIR:-$HOME/haxelib-dev/lime-832}"
HXCPP_DIR="${HXCPP_DIR:-$HOME/haxelib/hxcpp/4,3,2}"

export MACOSX_DEPLOYMENT_TARGET=11.0

case "${1:-}" in
	--check)
		cd "$PROJECT"
		./metal/scripts/check-compile.sh --lime "$LIME_DIR" --hxcpp "$HXCPP_DIR" --out /tmp/cne-check
		;;
	--bgfx-only)
		cd "$PROJECT"
		./metal/scripts/build-bgfx.sh
		;;
	*)
		# 1) install the backend into the Lime checkout and build the cpp ndll
		cd "$PROJECT"
		./metal/scripts/apply.sh --lime "$LIME_DIR" --build
		# 2) build the game (Haxe -> cpp); this overwrites the bundle ndll with the
		#    neko CLI bootstrap, so deploy the real one again afterwards
		haxelib run lime build mac
		./metal/scripts/apply.sh --lime "$LIME_DIR" --build --deploy
		echo
		echo "Done: $PROJECT/export/release/macos/bin/CodenameEngine.app"
		;;
esac
