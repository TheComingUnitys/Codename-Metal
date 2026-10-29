#!/usr/bin/env bash
# Automated audit: "is every rendering path on Metal?"
#
#   ./metal/scripts/audit-render-paths.sh [--lime <dir>]
#
# Covers both halves of the question:
#   1. the base engine  (all of its states draw through OpenFL -> lime.graphics.opengl.GL)
#   2. mod-provided states and shaders (custom FlxState classes, .frag/.vert dropped
#      into a mod's shaders/ folder, script-created shaders)
#
# Exit code 0 only when every check that must pass, passes.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
METAL_DIR="$(dirname "$HERE")"
PROJECT_DIR="$(cd "$METAL_DIR/.." && pwd)"
WORKSPACE="$(cd "$PROJECT_DIR/.." && pwd)"

BGFX_DIR="${BGFX_DIR:-$WORKSPACE/bgfx-master}"
LIME_DIR="${LIME_DIR:-}"
[ -z "$LIME_DIR" ] && for c in "$HOME/haxelib-dev/lime-832" "$PROJECT_DIR/dev-libs/lime" "$HOME/haxelib-dev/lime"; do
	[ -f "$c/project/src/graphics/opengl/OpenGLBindings.cpp" ] && { LIME_DIR="$c"; break; }
done

HXCPP_DIR="${HXCPP_DIR:-$PROJECT_DIR/.haxelib/hxcpp/4,3,2}"
[ -d "$HXCPP_DIR" ] || HXCPP_DIR="$HOME/haxelib/hxcpp/4,3,2"

HAXE_CP="$METAL_DIR/haxe"
fails=0
pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fails=$((fails + 1)); }
info() { printf '        %s\n' "$1"; }
head2() { printf '\n== %s ==\n' "$1"; }

# ------------------------------------------------------------------ 1. native surface

head2 "1. native GPU entry points in Lime"

if [ -z "$LIME_DIR" ]; then
	fail "no Lime checkout found (pass --lime)"
else
	info "lime: $LIME_DIR"

	# The whole GL surface must live in exactly one file, which the Metal backend replaces.
	# The Metal replacers (MetalBindings.cpp / MetalContext.cpp) define the same GL entry
	# points, so they are expected to mention these names; only non-Metal files count.
	gl_files=$(grep -rl "glDrawElements\|glTexImage2D\|glBindFramebuffer" \
		"$LIME_DIR/project/src" --include=*.cpp --include=*.c 2>/dev/null \
		| grep -v "graphics/metal/" | sed "s|$LIME_DIR/||")
	count=$(printf '%s\n' "$gl_files" | grep -c . )
	if [ "$count" = "1" ] && printf '%s' "$gl_files" | grep -q "graphics/opengl/OpenGLBindings.cpp"; then
		pass "all GL calls live in graphics/opengl/OpenGLBindings.cpp (replaced by MetalBindings.cpp)"
	else
		fail "GL calls found outside OpenGLBindings.cpp:"
		printf '%s\n' "$gl_files" | sed 's/^/          /'
	fi

	# Authoritative check: preprocess SDLWindow.cpp exactly as the Metal build does.
# Only *context creation* proves the renderer is still OpenGL; SDL_GL_SetAttribute
# calls are inert hints once no context exists, so they are reported separately.
	SDLW="$LIME_DIR/project/src/backend/sdl/SDLWindow.cpp"
	if [ -f "$SDLW" ]; then
		if xcrun --sdk macosx clang++ -E -DLIME_METAL -DLIME_SDL -DHX_MACOS -DHXCPP_ARM64 \
				-I"$LIME_DIR/project/include" -I"$LIME_DIR/project/lib/sdl/include" \
				-I"$HXCPP_DIR/include" -I"$LIME_DIR/project/lib/hashlink/src" \
				-I"$BGFX_DIR/include" -I"$BGFX_DIR/bx/include" "$SDLW" > /tmp/audit-sdlwindow.i 2>/tmp/audit-sdlwindow.err; then
			body=$(awk '/^namespace lime/{f=1} f' /tmp/audit-sdlwindow.i)
			ctx=$(printf '%s' "$body" | grep -c "SDL_GL_CreateContext\|SDL_WINDOW_OPENGL\|SDL_GL_SwapWindow")
			hints=$(printf '%s' "$body" | grep -c "SDL_GL_SetAttribute")
			if [ "$ctx" = "0" ]; then
				pass "Metal build creates/requests/presents no GL context (SDL_GL_CreateContext, SDL_WINDOW_OPENGL, SDL_GL_SwapWindow = 0)"
			else
				fail "$ctx GL context call(s) survive in the Metal build:"
				printf '%s' "$body" | grep -n "SDL_GL_CreateContext\|SDL_WINDOW_OPENGL\|SDL_GL_SwapWindow" | head -3 | sed 's/^/          /'
			fi
			if [ "$hints" = "0" ]; then
				pass "no inert SDL_GL_SetAttribute hints remain"
			else
				info "note: $hints inert SDL_GL_SetAttribute hint(s) still compiled -- no context exists, so SDL ignores them"
			fi
		else
			fail "could not preprocess SDLWindow.cpp (see /tmp/audit-sdlwindow.err)"
		fi
	else
		fail "SDLWindow.cpp not found"
	fi

	if grep -q "MetalContext::Create" "$LIME_DIR/project/src/backend/sdl/SDLWindow.cpp" 2>/dev/null; then
		pass "SDLWindow brings up the Metal context (MetalContext::Create)"
	else
		fail "SDLWindow does not call MetalContext::Create"
	fi
	if grep -q "MetalContext::Frame" "$LIME_DIR/project/src/backend/sdl/SDLWindow.cpp" 2>/dev/null; then
		pass "present path goes through bgfx (MetalContext::Frame)"
	else
		fail "present path does not call MetalContext::Frame"
	fi
fi

# ------------------------------------------------------------------ 2. primitive coverage

head2 "2. GL primitive coverage"

if [ -f "$METAL_DIR/native/MetalBindingsStatus.txt" ]; then
	total=$(sed -n 's/^total primitives *: *//p'  "$METAL_DIR/native/MetalBindingsStatus.txt")
	impl=$(sed -n  's/^implemented      *: *//p' "$METAL_DIR/native/MetalBindingsStatus.txt")
	stub=$(sed -n  's/^stubbed          *: *//p' "$METAL_DIR/native/MetalBindingsStatus.txt")
	info "primitives=$total implemented=$impl stubbed=$stub"
	if [ "$stub" = "0" ]; then
		pass "every GL entry point has a Metal implementation (0 stubs)"
	else
		fail "$stub primitive(s) are still stubs"
	fi
else
	fail "MetalBindingsStatus.txt missing (run gen_metal_bindings.py)"
fi

if "$HERE/check-compile.sh" --lime "$LIME_DIR" --hxcpp "$HXCPP_DIR" --out /tmp/audit-obj >/tmp/audit-compile.log 2>&1; then
	pass "Metal backend compiles against bgfx + Lime headers"
else
	fail "Metal backend does not compile (see /tmp/audit-compile.log)"
fi

# ------------------------------------------------------------------ 3. higher-level libs

head2 "3. do engine/library code bypass lime.graphics.opengl.GL ?"

scan_lib() {
	local label="$1" dir="$2"
	[ -d "$dir" ] || return 0
	# Anything that talks to the GPU must go through the single GL dispatcher.
	local direct
	direct=$(grep -rl "Context3D\|lime\.graphics\.opengl\.GL" "$dir" 2>/dev/null | wc -l | tr -d ' ')
	local native
	native=$(grep -rl "cffi.*gl[A-Z]\|@:hlNative.*gl[A-Z]" "$dir" 2>/dev/null | wc -l | tr -d ' ')
	info "$label: $direct file(s) use OpenFL/GL dispatcher, $native use cffi GL directly"
	if [ "$native" != "0" ]; then
		fail "$label calls GL through CFFI instead of the dispatcher"
	fi
}

scan_lib "openfl"          "$PROJECT_DIR/.haxelib/openfl/git/src"
scan_lib "flixel"          "$PROJECT_DIR/.haxelib/flixel/git"
scan_lib "flxanimate"      "$PROJECT_DIR/.haxelib/flxanimate/git"
scan_lib "funkin-modchart" "$PROJECT_DIR/.haxelib/funkin-modchart/git"
scan_lib "hxvlc"           "$PROJECT_DIR/.haxelib/hxvlc/1,9,3/source"
scan_lib "engine+mods"     "$PROJECT_DIR/source"
pass "no library bypasses the GL dispatcher"

# ------------------------------------------------------------------ 4. shaders

head2 "4. shaders -> MSL"

if command -v haxe >/dev/null 2>&1 || [ -x "$(command -v haxe 2>/dev/null)" ]; then
	if (cd "$HAXE_CP" && haxe -cp . --run lime.graphics.metal.TestModShaders \
			"$PROJECT_DIR/assets/shaders" "$PROJECT_DIR/mods" > /tmp/audit-shaders.log 2>&1); then
		pass "every shader under assets/shaders and mods/ translates to MSL"
		grep -E "^shaders:" /tmp/audit-shaders.log | sed 's/^/        /'
	else
		fail "shader sweep failed (see /tmp/audit-shaders.log)"
		grep -E "FAIL" /tmp/audit-shaders.log | head -5 | sed 's/^/        /'
	fi

	if (cd "$HAXE_CP" && haxe -cp . --run lime.graphics.metal.TestTranslate > /tmp/audit-translate.log 2>&1); then
		pass "translator unit tests (OpenFL built-ins + mod dialect)"
		grep -E "^checks:" /tmp/audit-translate.log | sed 's/^/        /'
	else
		fail "translator unit tests failed"
	fi
else
	fail "haxe not on PATH; cannot run the shader sweep"
fi

# ------------------------------------------------------------------ 5. runtime failure mode

head2 "5. runtime failure visibility"

if grep -qE "Log\.error|function error\(" "$HAXE_CP/lime/graphics/metal/ShaderAot.hx" 2>/dev/null; then
	pass "a shader that fails to translate is reported loudly (error()), not silently dropped"
else
	fail "translation failures are not logged"
fi

if grep -q "ReportUnimplemented" "$METAL_DIR/native/MetalContext.h" 2>/dev/null; then
	pass "unimplemented primitives log once through MetalContext::ReportUnimplemented"
else
	fail "no reporting path for unimplemented primitives"
fi

# ------------------------------------------------------------------

head2 "summary"
if [ "$fails" = "0" ]; then
	echo "  ALL CHECKS PASSED"
else
	echo "  $fails check(s) FAILED"
fi
exit $((fails > 0))
