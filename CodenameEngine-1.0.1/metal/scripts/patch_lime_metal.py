#!/usr/bin/env python3
"""
Apply every native-side change the bgfx/Metal backend needs to a Lime checkout.

    patch_lime_metal.py --lime <dir> [--bgfx <dir>] [--bgfx-lib <dir>]
                        [--dry-run] [--revert]

Everything here was found by actually making the Metal build work end to end; each
edit is idempotent and fails loudly (file + expected anchor) instead of guessing.

  1. project/Build.xml            -> compile flags, Metal sources, bgfx link flags
                                     (delegated to patch_lime_build.py)
  2. src/backend/sdl/SDLWindow.cpp-> Metal window: SDL_WINDOW_METAL, MetalContext
                                     create/present/vsync, and the GL probe block
                                     compiled out
  3. lib/custom/sdl/include/*     -> SDL_VIDEO_METAL: this pre-generated config is
                                     FIRST on the include path and never defined it,
                                     so Cocoa's Metal registration was compiled out
  4. lib/sdl-files.xml            -> -D__MACOSX__ (SDL_config.h only picks
                                     SDL_config_macosx.h when it is defined)
  5. lib/pixman/.../pixman-implementation.c
                                  -> guard the RISC-V call (Lime's file list does not
                                     compile pixman-riscv.c)
  6. lib/openal-files.xml         -> OpenAL Soft's bundled fmt/gsl include paths and
                                     -std=c++20 (its GSL needs it)
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def log(msg):
    print("  " + msg)


def die(msg):
    print("patch_lime_metal: error: " + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        die("cannot read %s (%s)" % (path, e))


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def sub_once(text, old, new, label, path, dry):
    """Replace `old` with `new`; no-op if `new`'s marker already present."""
    if new in text:
        log("already applied: %s" % label)
        return text
    if old not in text:
        die("anchor not found for '%s' in %s\n  expected: %r" % (label, path, old.strip()[:120]))
    log("applied: %s" % label)
    return text.replace(old, new, 1)


# --------------------------------------------------------------------- 2. SDLWindow

def patch_sdlwindow(lime, dry):
    path = os.path.join(lime, "project/src/backend/sdl/SDLWindow.cpp")
    src = read(path)
    orig = src

    src = sub_once(
        src,
        '#include "../../graphics/opengl/OpenGLBindings.h"',
        '#include "../../graphics/opengl/OpenGLBindings.h"\n'
        '#ifdef LIME_METAL\n'
        '#include "../../graphics/metal/MetalContext.h"   // CNE_METAL_WINDOW_PATCH\n'
        '#endif',
        "SDLWindow: include MetalContext", path, dry)

    if "SDL_WINDOW_METAL" in src:
        log("already applied: SDLWindow: window flags")
    else:
        src = sub_once(
            src,
            '\t\t\tsdlWindowFlags |= SDL_WINDOW_OPENGL;',
            '#ifndef LIME_METAL\n'
        '\t\t\tsdlWindowFlags |= SDL_WINDOW_OPENGL;\n'
        '#else\n'
        '\t\t\t// SDL3 needs the window flagged Metal-capable before\n'
        '\t\t\t// SDL_Metal_CreateView() will hand out a CAMetalLayer.\n'
        '\t\t\tsdlWindowFlags |= SDL_WINDOW_METAL;\n'
        '#endif',
        "SDLWindow: window flags", path, dry)

    src = sub_once(
        src,
        '\t\t\tcontext = SDL_GL_CreateContext (sdlWindow);',
        '#ifdef LIME_METAL\n'
        '\t\t\tlime::metal::MetalContext::Create (sdlWindow, width, height,\n'
        '\t\t\t\t(flags & WINDOW_FLAG_ALLOW_HIGHDPI) != 0);\n'
        '#else\n'
        '\t\t\tcontext = SDL_GL_CreateContext (sdlWindow);\n'
        '#endif',
        "SDLWindow: create context", path, dry)

    src = sub_once(
        src,
        '\t\t\tSDL_GL_SwapWindow (sdlWindow);',
        '#ifdef LIME_METAL\n'
        '\t\t\tlime::metal::MetalContext::Frame ();\n'
        '#else\n'
        '\t\t\tSDL_GL_SwapWindow (sdlWindow);\n'
        '#endif',
        "SDLWindow: present", path, dry)

    # GetContextType() must report "opengl": Lime's Haxe side uses it to decide whether
    # to build a NativeOpenGLRenderContext at all.  With context == null it fell through
    # to "none", OpenFL never created a renderer and no draw call was ever issued.
    if "CNE_METAL_CONTEXT_TYPE" not in src:
        old_ctx = """	const char* SDLWindow::GetContextType () {

		if (context) {"""
        new_ctx = """	const char* SDLWindow::GetContextType () {

#ifdef LIME_METAL
		// CNE_METAL_CONTEXT_TYPE
		return "opengl";
#endif

		if (context) {"""
        if old_ctx not in src:
            die("anchor not found for 'SDLWindow: GetContextType' in %s" % path)
        src = src.replace(old_ctx, new_ctx, 1)
        log("applied: SDLWindow: GetContextType -> \"opengl\" on the Metal path")
    else:
        log("already applied: SDLWindow: GetContextType")

    # The present path must not be gated on a GL context: on the Metal build
    # `context` is always null, so bgfx::frame() was never called and the window
    # never presented.
    if "CNE_METAL_PRESENT" not in src:
        old_flip = """	void SDLWindow::ContextFlip () {

		if (context && !sdlRenderer) {

#ifdef LIME_METAL
			lime::metal::MetalContext::Frame ();
#else
			SDL_GL_SwapWindow (sdlWindow);
#endif

		} else if (sdlRenderer) {

			SDL_RenderPresent (sdlRenderer);

		}

	}"""
        new_flip = """	void SDLWindow::ContextFlip () {

#ifdef LIME_METAL
		// CNE_METAL_PRESENT
		lime::metal::MetalContext::Frame ();
#else
		if (context && !sdlRenderer) {

			SDL_GL_SwapWindow (sdlWindow);

		} else if (sdlRenderer) {

			SDL_RenderPresent (sdlRenderer);

		}
#endif

	}"""
        if old_flip not in src:
            die("anchor not found for 'SDLWindow: present path' in %s" % path)
        src = src.replace(old_flip, new_flip, 1)
        log("applied: SDLWindow: present path (bgfx::frame on the Metal path)")
    else:
        log("already applied: SDLWindow: present path")

    if "CNE_METAL_NO_GL_BLOCK" not in src:
        key = "if (context && SDL_GL_MakeCurrent (sdlWindow, context) == 0) {"
        if key not in src:
            die("anchor not found for 'SDLWindow: GL probe block' in %s" % path)

        def block_end(start):
            d = 0
            for j in range(start, len(src)):
                if src[j] == '{':
                    d += 1
                elif src[j] == '}':
                    d -= 1
                    if d == 0:
                        return j
            return -1

        i = src.index(key)
        j = block_end(i)
        k = j + 1
        while k < len(src) and src[k] in ' \t\r\n':
            k += 1
        if src.startswith('else', k):
            j = block_end(src.index('{', k))

        head = ('#ifdef LIME_METAL\n'
                '\t\t\t// CNE_METAL_NO_GL_BLOCK\n'
                '\t\t\t// No GL context exists on the Metal path, and OpenGLBindings.cpp is not\n'
                '\t\t\t// part of this build, so the GL probe must not be compiled.  VSync is\n'
                '\t\t\t// owned by bgfx.\n'
                '\t\t\tlime::metal::MetalContext::SetVsync ((flags & WINDOW_FLAG_VSYNC) != 0);\n'
                '#else\n')
        src = src[:i] + head + src[i:j + 1] + '\n#endif\n' + src[j + 1:]
        log("applied: SDLWindow: GL probe block guarded")
    else:
        log("already applied: SDLWindow: GL probe block guarded")

    if not dry and src != orig:
        write(path, src)


# ------------------------------------------------------------------ 3. custom/sdl

def patch_custom_sdl(lime, dry):
    base = os.path.join(lime, "project/lib/custom/sdl/include")
    if not os.path.isdir(base):
        log("skip: %s not present (not a Lime checkout with custom SDL config)" % base)
        return

    add = ('\n/* CNE local fix: this pre-generated Lime SDL config never defined\n'
           '   SDL_VIDEO_METAL, so SDL_cocoavideo.m compiled its Metal registration out\n'
           '   and SDL_Metal_CreateView() could never be used on macOS.\n'
           '   NOTE: lib/custom/sdl/include comes FIRST on the include path, so this file\n'
           '   shadows lib/sdl/include/SDL_config.h. */\n'
           '#ifndef SDL_VIDEO_METAL\n#define SDL_VIDEO_METAL 1\n#endif\n'
           '#ifndef SDL_PLATFORM_SUPPORTS_METAL\n#define SDL_PLATFORM_SUPPORTS_METAL 1\n#endif\n')

    for name in ("SDL_config_macosx.h", "SDL_build_config.h"):
        path = os.path.join(base, name)
        if not os.path.isfile(path):
            continue
        src = read(path)
        if "SDL_VIDEO_METAL" in src:
            log("already applied: custom/sdl/%s" % name)
            continue
        log("applied: custom/sdl/%s (SDL_VIDEO_METAL=1)" % name)
        if not dry:
            write(path, src + add)


# ------------------------------------------------------------------ 4. sdl-files.xml

def patch_sdl_files(lime, dry):
    path = os.path.join(lime, "project/lib/sdl-files.xml")
    src = read(path)
    if "__MACOSX__" in src:
        log("already applied: sdl-files.xml -D__MACOSX__")
        return
    i = src.index("SDL_cocoavideo.m")
    j = src.rindex("<files id=", 0, i)
    end = src.index(">", j) + 1
    ins = ('\n\n        <!-- CNE local fix: SDL_config.h only selects SDL_config_macosx.h\n'
           '             when __MACOSX__ is defined. SDL\'s own build sets it; Lime did\n'
           '             not, so SDL fell back to SDL_config_minimal.h. -->\n'
           '        <compilerflag value="-D__MACOSX__" if="mac" />\n')
    log("applied: sdl-files.xml -D__MACOSX__")
    if not dry:
        write(path, src[:end] + ins + src[end:])


# ------------------------------------------------------------------ 5. pixman

def patch_pixman(lime, dry):
    path = os.path.join(lime, "project/lib/pixman/pixman/pixman-implementation.c")
    if not os.path.isfile(path):
        log("skip: pixman source not present")
        return
    src = read(path)
    old = "    imp = _pixman_riscv_get_implementations (imp);"
    if "__riscv" in src:
        log("already applied: pixman riscv guard")
        return
    if old not in src:
        die("anchor not found for 'pixman riscv guard' in %s" % path)
    new = ("#if defined(__riscv)\n"
           "    /* CNE local fix: Lime's pixman-files.xml does not compile\n"
           "       pixman-riscv.c, so this call is unresolved on other architectures. */\n"
           + old + "\n#endif")
    log("applied: pixman riscv guard")
    if not dry:
        write(path, src.replace(old, new, 1))


# ------------------------------------------------------------------ 6. openal

def patch_openal(lime, dry):
    path = os.path.join(lime, "project/lib/openal-files.xml")
    if not os.path.isfile(path):
        log("skip: openal-files.xml not present")
        return
    src = read(path)
    if "fmt-11.2.0/include" in src and "std=c++20" in src:
        log("already applied: openal fmt/gsl")
        return
    anchor = '<files id="native-toolkit-openal">'
    if anchor not in src:
        die("anchor not found for 'openal fmt/gsl' in %s" % path)
    ins = (anchor + '\n\n'
           '        <!-- CNE local fix: OpenAL Soft here vendors fmt/gsl, and its GSL\n'
           '             needs C++14/20 (std::span, enable_if_t). -->\n'
           '        <compilerflag value="-I${NATIVE_TOOLKIT_PATH}/openal/fmt-11.2.0/include" />\n'
           '        <compilerflag value="-I${NATIVE_TOOLKIT_PATH}/openal/gsl/include" />\n'
           '        <compilerflag value="-std=c++20" if="mac || linux" />\n')
    log("applied: openal fmt/gsl includes + -std=c++20")
    if not dry:
        write(path, src.replace(anchor, ins, 1))


# ------------------------------------------------------------------ 6b. openal backends
#
# Lime 8.3 ships OpenAL Soft without the `config.h` / `config_backends.h` that
# upstream generates with CMake, so this checkout carries hand-written copies under
# `project/lib/custom/openal/include/`.  The Apple copy had
#
#     #ifdef NATIVE_TOOLKIT_HAVE_SDL
#     #define HAVE_COREAUDIO 0      <-- no CoreAudio
#     #else
#     #define HAVE_COREAUDIO 1
#     #endif
#
# while `config_backends.h` also pins `HAVE_SDL2 0` and `HAVE_SDL3 0` (this Lime
# build uses SDL2, but OpenAL Soft 1.25's SDL backend expects SDL3).  Every backend
# was therefore off, and OpenAL Soft came up with the **null** backend only:
#
#     [ALSOFT] (II) Supported backends: null
#     [ALSOFT] (II) Created device ..., "No Output"
#
# The game then "played" every sound into the void - the mixer thread runs, the
# process looks healthy, and CoreAudio reports `runningOutput = 0` for it (see
# metal/tools/README.md).  CoreAudio is the correct macOS backend and has no extra
# dependencies.  Keep it on, unconditionally.

OPENAL_APPLE_BACKENDS = """\
/* CNE Metal backend: OpenAL Soft backend selection for macOS.
 *
 * Generated by metal/scripts/patch_lime_metal.py.  Lime 8.3 does not ship the
 * config_backends.h that upstream OpenAL Soft generates with CMake, so this file
 * is written by hand - and the hand-written version had CoreAudio switched off
 * whenever SDL was present, with the SDL backends off as well, which left OpenAL
 * with the `null` backend and the game completely silent.
 *
 * CoreAudio is the native macOS backend and needs no extra dependencies.
 */
#define HAVE_COREAUDIO 1

#define HAVE_ALSA 0

#define HAVE_OSS 0

#define HAVE_PIPEWIRE 0

#define HAVE_SOLARIS 0

#define HAVE_SNDIO 0

#define HAVE_WASAPI 0

#define HAVE_DSOUND 0

#define HAVE_WINMM 0

#define HAVE_PORTAUDIO 0

#define HAVE_PULSEAUDIO 0

#define HAVE_JACK 0

#define HAVE_OPENSL 0

#define HAVE_OBOE 0

/* This Lime build links SDL2; OpenAL Soft 1.25's SDL backend targets SDL3. */
#define HAVE_SDL2 0
#define HAVE_SDL3 0
"""


def patch_openal_backends(lime, dry):
    path = os.path.join(lime, "project/lib/custom/openal/include/platforms/apple/config_backends.h")
    good = "HAVE_COREAUDIO 1" in OPENAL_APPLE_BACKENDS

    if os.path.isfile(path):
        src = read(path)
        if "HAVE_COREAUDIO 1" in src and "HAVE_COREAUDIO 0" not in src:
            log("already applied: openal CoreAudio backend")
            return
    if not good:  # pragma: no cover - guards against a bad edit of the constant
        die("OPENAL_APPLE_BACKENDS does not enable CoreAudio")

    log("applied: openal CoreAudio backend (HAVE_COREAUDIO 1)")
    if not dry:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(OPENAL_APPLE_BACKENDS)


# ------------------------------------------------------------------ 6c. openal backend sources
#
# `Build.xml` sets `NATIVE_TOOLKIT_HAVE_SDL` whenever LIME_SDL is on, and
# `openal-files.xml` puts **every** real backend file inside
#
#     <section unless="NATIVE_TOOLKIT_HAVE_SDL">
#         ... coreaudio.cpp if="apple" ...
#     </section>
#
# on the assumption that the SDL backend takes over - except it never lists
# `sdl2.cpp` / `sdl3.cpp` at all (and this Lime vendors SDL 2 while OpenAL Soft
# 1.25's SDL backend wants SDL 3).  Net result: the ndll is built with **zero**
# playback backends, OpenAL Soft starts with `null`, and `alcOpenDevice` happily
# returns a device literally named "No Output" - which is why the game looks fine
# and makes no sound.
#
# Compile OpenAL Soft's CoreAudio backend alongside SDL instead.  It is the native
# macOS backend, needs only the AudioToolbox/AudioUnit/CoreAudio frameworks that
# the mac link section already pulls in, and does not go through SDL.

OPENAL_COREAUDIO_SECTION = """\
\t<!-- ==== CNE Metal backend: OpenAL Soft needs a real macOS backend ==== -->
\t<section if="NATIVE_TOOLKIT_HAVE_SDL">
\t\t<!-- See metal/scripts/patch_lime_metal.py step 6c: the section above is skipped
\t\t     whenever SDL is enabled and no SDL backend exists in this tree, which left
\t\t     OpenAL Soft with the `null` backend and the game silent. -->
\t\t<file name="${NATIVE_TOOLKIT_PATH}/openal/alc/backends/coreaudio.cpp" if="apple" />
\t</section>
\t<!-- ==== /CNE Metal backend ==== -->
"""


def patch_openal_backend_sources(lime, dry):
    path = os.path.join(lime, "project/lib/openal-files.xml")
    if not os.path.isfile(path):
        log("skip: openal-files.xml not present")
        return

    src = read(path)
    if "CNE Metal backend: OpenAL Soft needs a real macOS backend" in src:
        log("already applied: openal CoreAudio source")
        return

    # Land it just before the closing </files> of the native-toolkit-openal fileset.
    marker = "</files>"
    idx = src.find(marker)
    if idx < 0:
        die("cannot find </files> in %s" % path)

    log("applied: openal CoreAudio source (compiled when SDL is enabled)")
    if not dry:
        write(path, src[:idx] + OPENAL_COREAUDIO_SECTION + src[idx:])


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lime", required=True)
    ap.add_argument("--bgfx", required=True)
    ap.add_argument("--bgfx-lib", required=True)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    lime = args.lime.rstrip("/")
    if not os.path.isfile(os.path.join(lime, "project/Build.xml")):
        die("%s does not look like a Lime checkout" % lime)

    print("== 1. project/Build.xml ==")
    r = subprocess.run([sys.executable, os.path.join(HERE, "patch_lime_build.py"),
                        "--lime", lime, "--bgfx", args.bgfx, "--bgfx-lib", args.bgfx_lib],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        die("patch_lime_build.py failed")

    print("== 2. SDLWindow.cpp ==")
    patch_sdlwindow(lime, args.dry_run)

    print("== 3. custom/sdl config (SDL_VIDEO_METAL) ==")
    patch_custom_sdl(lime, args.dry_run)

    print("== 4. sdl-files.xml (-D__MACOSX__) ==")
    patch_sdl_files(lime, args.dry_run)

    print("== 5. pixman riscv guard ==")
    patch_pixman(lime, args.dry_run)

    print("== 6. openal fmt/gsl ==")
    patch_openal(lime, args.dry_run)

    print("== 6b. openal CoreAudio backend ==")
    patch_openal_backends(lime, args.dry_run)

    print("== 6c. openal CoreAudio source ==")
    patch_openal_backend_sources(lime, args.dry_run)

    print("\ndone%s" % (" (dry run)" if args.dry_run else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
