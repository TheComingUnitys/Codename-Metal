#!/usr/bin/env bash
# Codename Engine - install the Metal/bgfx rendering backend into a Lime checkout.
#
#   ./metal/scripts/apply.sh [--lime <path>] [--build]
#
# What it does (idempotent):
#   1. copies metal/native/*.cpp|h into <lime>/project/src/graphics/metal/
#   1b. regenerates the AOT (build-time) GLSL->MSL->.metallib shader cache
#       (metal/haxe/lime/graphics/metal/ShaderAotCache.hx) so the game never
#       translates nor compiles shaders while it runs -- see
#       metal/audit/aot-shaders.md and metal/audit/metallib.md
#   2. appends the Metal section to <lime>/project/Build.xml (once)
#   3. copies the Haxe shim (Translator + render context override) into
#      <lime>/src/lime/graphics/metal/  and  <lime>/src/lime/_internal/backend/native/
#   4. copies the engine-side overrides into CodenameEngine-1.0.1/source/lime/...
#   5. with --build, runs `haxelib run lime rebuild mac`
#   6. with --deploy, installs the freshly built cpp ndll into the .app bundle
#      (plain `lime build mac` puts Lime's *neko CLI bootstrap* ndll there)
#
# The patch is intentionally *additive*: nothing is deleted, so reverting means
# removing the copied files and the marked Build.xml block.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
METAL_DIR="$(dirname "$HERE")"
PROJECT_DIR="$(cd "$METAL_DIR/.." && pwd)"

LIME_DIR="${LIME_DIR:-}"
BGFX_DIR="${BGFX_DIR:-$(cd "$PROJECT_DIR/.." && pwd)/bgfx-master}"
BGFX_LIB_DIR="${BGFX_LIB_DIR:-$BGFX_DIR/.build/osx-arm64/bin}"
DO_BUILD=0
DO_AOT=1
DO_METALLIB=1
DO_DEPLOY=0
NO_DEPLOY=0
APP="${APP:-$PROJECT_DIR/export/release/macos/bin/CodenameEngine.app}"

while [ $# -gt 0 ]; do
	case "$1" in
		--lime) LIME_DIR="$2"; shift 2 ;;
		--build) DO_BUILD=1; shift ;;
		--skip-aot) DO_AOT=0; shift ;;
		--skip-metallib) DO_METALLIB=0; shift ;;
		--deploy) DO_DEPLOY=1; shift ;;
		--no-deploy) NO_DEPLOY=1; shift ;;
		--) shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

if [ -z "$LIME_DIR" ]; then
	for candidate in "$PROJECT_DIR/dev-libs/lime" "$HOME/haxelib-dev/lime-832" "$HOME/haxelib-dev/lime-812"; do
		if [ -f "$candidate/project/src/graphics/opengl/OpenGLBindings.cpp" ]; then
			LIME_DIR="$candidate"
			break
		fi
	done
fi

if [ -z "$LIME_DIR" ] || [ ! -f "$LIME_DIR/project/src/graphics/opengl/OpenGLBindings.cpp" ]; then
	echo "error: could not find a Lime checkout; pass --lime <path>" >&2
	exit 2
fi

echo "metal backend : $METAL_DIR"
echo "lime          : $LIME_DIR"

# ---------------------------------------------------------------- 1. native sources

NATIVE_DST="$LIME_DIR/project/src/graphics/metal"
mkdir -p "$NATIVE_DST"
# copy every native source/fragment the backend is made of (the hand-written
# MetalUniforms/MetalTextures/MetalStates.inc fragments included)
for f in "$METAL_DIR"/native/*.h "$METAL_DIR"/native/*.cpp "$METAL_DIR"/native/*.mm "$METAL_DIR"/native/*.inc; do
	[ -f "$f" ] && cp "$f" "$NATIVE_DST/"
done
touch "$NATIVE_DST/.keep"
echo "  [1/6] native sources -> project/src/graphics/metal/"

# ---------------------------------------------------------------- 1b. AOT shaders
#
# The runtime never translates GLSL: `MetalShaderBridge.onShaderSource` only hashes
# the assembled source and looks the MSL up in the build-time cache.  This step
# regenerates that cache from the current Haxe sources + assets, so a build is always
# self-consistent without a manual step.  It needs only `haxe` (the translator runs
# through `haxe --interp`), not the C++ toolchain.
if [ "$DO_AOT" = "1" ]; then
	AOT_ARGS=(--project "$PROJECT_DIR")
	[ "$DO_METALLIB" = "1" ] || AOT_ARGS+=(--skip-metallib)
	python3 "$HERE/gen_shader_aot.py" "${AOT_ARGS[@]}" || {
		echo "error: AOT shader cache generation failed" >&2; exit 1; }
	echo "  [1b/6] AOT GLSL->MSL(+.metallib) shader cache regenerated"
else
	echo "  [1b/6] AOT shader cache generation skipped (--skip-aot)"
fi

# ---------------------------------------------------------------- 2. Haxe shim

HAXE_SRC="$METAL_DIR/haxe/lime/graphics/metal"
HAXE_DST="$LIME_DIR/src/lime/graphics/metal"
mkdir -p "$HAXE_DST"
cp "$HAXE_SRC"/*.hx "$HAXE_DST/"
rm -f "$HAXE_DST/TestTranslate.hx"
echo "  [2/6] GLSL->MSL translator + MetalShaderBridge -> src/lime/graphics/metal/"

# ---------------------------------------------------------------- 3. Build.xml section
#
# Adds `unless="LIME_METAL"` to the OpenGLBindings section and appends the Metal
# section (sources + bgfx/Metal link flags).  Logic lives in patch_lime_build.py.

python3 "$HERE/patch_lime_metal.py" \
	--lime "$LIME_DIR" \
	--bgfx "$BGFX_DIR" \
	--bgfx-lib "$BGFX_LIB_DIR" || exit 1
echo "  [3/6] Lime native patches applied (Build.xml / SDLWindow / SDL config / pixman / openal)"

# ---------------------------------------------------------------- 3b. SDLWindow patch

SDL_WINDOW_CPP="$LIME_DIR/project/src/backend/sdl/SDLWindow.cpp"
if [ -f "$SDL_WINDOW_CPP" ] && ! grep -qF "CNE_METAL_WINDOW_PATCH" "$SDL_WINDOW_CPP"; then
	python3 - "$SDL_WINDOW_CPP" <<-'PY'
		import sys
		path = sys.argv[1]
		src = open(path).read()

		# 1. include the Metal context
		src = src.replace('#include "../../graphics/opengl/OpenGLBindings.h"',
		                  '#include "../../graphics/opengl/OpenGLBindings.h"\n'
		                  '#ifdef LIME_METAL\n'
		                  '#include "../../graphics/metal/MetalContext.h"   // CNE_METAL_WINDOW_PATCH\n'
		                  '#endif', 1)

		# 2. do not ask SDL for a GL context in the Metal configuration
		src = src.replace('\t\t\tsdlWindowFlags |= SDL_WINDOW_OPENGL;',
		                  '#ifndef LIME_METAL\n'
		                  '\t\t\tsdlWindowFlags |= SDL_WINDOW_OPENGL;\n'
		                  '#endif', 1)

		# 3. bring up bgfx instead of SDL_GL_CreateContext
		old = '\t\t\tcontext = SDL_GL_CreateContext (sdlWindow);'
		new = ('#ifdef LIME_METAL\n'
		       '\t\t\tlime::metal::MetalContext::Create (sdlWindow, width, height,\n'
		       '\t\t\t\t(flags & WINDOW_FLAG_ALLOW_HIGHDPI) != 0);\n'
		       '#else\n'
		       + old + '\n'
		       '#endif')
		src = src.replace(old, new, 1)

		# 4. present through bgfx
		old_swap = '\t\t\tSDL_GL_SwapWindow (sdlWindow);'
		new_swap = ('#ifdef LIME_METAL\n'
		            '\t\t\tlime::metal::MetalContext::Frame ();\n'
		            '#else\n'
		            + old_swap + '\n'
		            '#endif')
		src = src.replace(old_swap, new_swap, 1)

		open(path, "w").write(src)
	PY
	echo "  [3b/6] SDLWindow.cpp patched for LIME_METAL"
else
	echo "  [3b/6] SDLWindow.cpp already patched (or not found)"
fi

# ---------------------------------------------------------------- 3b2. menu pump
# AppKit's window-tiling hover menu over the green button runs a nested event-tracking loop;
# SDL_WaitEvent() does not return while it is open.  The patch exposes a direct
# UPDATE+RENDER pump that MetalLayerHost.mm calls from an NSRunLoopCommonModes timer.
if [ -f "$HERE/patch_lime_menu_pump.py" ]; then
	python3 "$HERE/patch_lime_menu_pump.py" --lime "$LIME_DIR" || {
		echo "error: patch_lime_menu_pump.py failed" >&2; exit 1; }
fi

# ---------------------------------------------------------------- 3c. Haxe hooks

# `patch_lime_haxe.py` wires Lime's GL entry points to the GLSL->MSL translator
# (MetalShaderBridge) and fixes the GLSL version on the Metal path.
OPENFL_DIR="${OPENFL_DIR:-$PROJECT_DIR/.haxelib/openfl/git}"
if [ -f "$HERE/patch_lime_haxe.py" ]; then
	if [ -d "$OPENFL_DIR" ]; then
		python3 "$HERE/patch_lime_haxe.py" --lime "$LIME_DIR" --openfl "$OPENFL_DIR" || {
			echo "error: patch_lime_haxe.py failed" >&2; exit 1; }
	else
		python3 "$HERE/patch_lime_haxe.py" --lime "$LIME_DIR" --openfl-missing=skip || {
			echo "error: patch_lime_haxe.py failed" >&2; exit 1; }
	fi
	echo "  [3c/6] Haxe hooks applied (MetalShaderBridge + GLSL version)"
else
	echo "  [3c/5] patch_lime_haxe.py not found - skipped"
fi

# ---------------------------------------------------------------- 3d. native Metal 2D path (Phase 1)
#
# Adds the env-gated (`CNE_METAL_NATIVE_PATH`) native path hooks to OpenFL's
# Context3D / OpenGLRenderer.  Purely additive: every hook keeps the original GL
# call in its `else` branch, so with the env var unset the old path runs.
if [ -f "$HERE/patch_openfl_native.py" ] && [ -d "$OPENFL_DIR" ]; then
	python3 "$HERE/patch_openfl_native.py" --openfl "$OPENFL_DIR" || {
		echo "error: patch_openfl_native.py failed" >&2; exit 1; }
	echo "  [3d/6] OpenFL native-path hooks applied (Phase 1: state stage)"
fi

# ---------------------------------------------------------------- 4. engine overrides

ENGINE_DST="$PROJECT_DIR/source/lime/graphics/metal"
mkdir -p "$ENGINE_DST"
cp "$HAXE_SRC"/*.hx "$ENGINE_DST/"
rm -f "$ENGINE_DST/TestTranslate.hx"
echo "  [4/6] translator mirrored into source/lime/graphics/metal/"

# ---------------------------------------------------------------- 5. rebuild
#
# The `cd "$PROJECT_DIR"` below is load-bearing, not cosmetic.
#
# `lime rebuild mac` only walks `project.haxelibs` when it runs against a Haxe
# project that lists them; it then rebuilds each haxelib's native library through
# that haxelib's `project.rebuild.path`.  Run from inside the Lime checkout
# instead and Lime's own HXProject has no `project.rebuild.path`, so
# `CPPHelper.rebuildSingle()` returns silently at its `if (path == null) return;`
# guard: exit code 0, no output, no `ndll/MacArm64/lime-64.ndll` - which looks
# exactly like a successful no-op build.  From the engine directory the `lime`
# haxelib (dev'd to ~/haxelib-dev/lime-832) *is* a project dependency, so its
# native library is the one that gets rebuilt.
#
# Adding `-Dmetal` on the command line is optional here (`project.xml` defines it)
# but harmless; keep it if you invoke the rebuild by hand.

if [ "$DO_BUILD" = "1" ]; then
	echo "  [5/6] haxelib run lime rebuild mac"
	(cd "$PROJECT_DIR" && haxelib run lime rebuild mac)
else
	echo "  [5/6] skipped (pass --build to run 'haxelib run lime rebuild mac')"
fi

# ---------------------------------------------------------------- 6. deploy ndll
#
# A real gotcha in this setup: `haxelib run lime build mac` copies
# `<lime>/ndll/MacArm64/lime.ndll` into the bundle, and that file is the *neko CLI
# bootstrap* ndll (it has no `lime_metal_*` symbols), not the hxcpp build produced by
# `lime rebuild mac` (`lime-64.ndll`).  Launching the bundle with the wrong ndll
# reports "Metal (Metal: yes)" only if the backend is in the cpp ndll -- it is not,
# so the app aborts.  Re-install the right one and re-sign (macOS SIGKILLs an app
# whose bundle signature no longer matches after a file was replaced).

if [ "$DO_DEPLOY" = "1" ]; then
	APP="$PROJECT_DIR/export/release/macos/bin/CodenameEngine.app"
	if [ -d "$APP" ] && [ -f "$LIME_DIR/ndll/MacArm64/lime-64.ndll" ]; then
		cp "$LIME_DIR/ndll/MacArm64/lime-64.ndll" "$APP/Contents/MacOS/lime.ndll"
		codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true
		echo "  [6/6] deployed $(basename "$LIME_DIR")/ndll/MacArm64/lime-64.ndll into the app bundle"
	else
		echo "  [6/6] deploy skipped (no app bundle at $APP)"
	fi
else
	echo "  [6/6] deploy skipped (pass --deploy after 'lime build mac')"
fi

# ---------------------------------------------------------------- safety net
# CNE_APPLY_SAFETY_V1
#
# `haxelib run lime build mac` installs Lime's *neko CLI bootstrap* ndll into the
# bundle, replacing the freshly built cpp ndll.  Launching the app in that state
# aborts at startup, and it is silent -- the only symptom is a smaller ndll
# (~7.9 MB instead of ~10.3 MB).  Forgetting the final `--deploy` has already
# shipped one broken bundle, so: after ANY --build, deploy automatically, and
# then verify the bundle's ndll is really the cpp one (size ratio vs the built
# artefact; the neko bootstrap is ~77% of it).
if [ "$DO_BUILD" = 1 ] && [ "$NO_DEPLOY" != 1 ] && [ -f "$APP/Contents/MacOS/lime.ndll" ]; then
	SRC="$LIME_DIR/ndll/MacArm64/lime-64.ndll"
	DST="$APP/Contents/MacOS/lime.ndll"
	if [ -f "$SRC" ] && [ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" != "$(shasum -a 256 "$DST" | cut -d' ' -f1)" ]; then
		echo "  [safety] restoring the cpp ndll (lime build mac put the neko bootstrap there)"
		cp "$SRC" "$DST" && codesign --force --deep --sign - "$APP" >/dev/null 2>&1
	fi
	if [ -f "$SRC" ]; then
		A=$(stat -f%z "$DST" 2>/dev/null || echo 0)
		B=$(stat -f%z "$SRC" 2>/dev/null || echo 0)
		if [ "$B" -gt 0 ] && [ $((A * 100 / B)) -lt 90 ]; then
			echo "!! [safety] app ndll is ${A}B but the built artefact is ${B}B -- the bundle is holding Lime's neko bootstrap and WILL ABORT AT STARTUP." >&2
			echo "!! [safety] fix with: ./metal/scripts/apply.sh --lime $LIME_DIR --deploy" >&2
			exit 1
		fi
		echo "  [safety] app ndll verified: ${A}B (built ${B}B, SIGNED)"
	fi
fi
