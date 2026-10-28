#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_lime_haxe.py -- install the Codename Engine "Metal/bgfx" Haxe hooks.

The Metal backend keeps `lime.graphics.opengl.GL` (and therefore OpenFL /
HaxeFlixel / Codename Engine) 100% untouched.  Only the four places where Lime
talks to the native GL implementation are hooked, all guarded by `#if metal`,
and all routed through `lime.graphics.metal.MetalShaderBridge` (installed by
`metal/scripts/apply.sh`) which turns GLSL into MSL and then calls the native
`lime_metal_configure_program` primitive.

Patched files
-------------
1. <lime>/src/lime/_internal/backend/native/NativeCFFI.hx
     + lime_metal_configure_program(program, attributeMap, uniformMap) : Dynamic
     + lime_metal_is_metal() : Int
   in the `cpp` (both the `@:cffi` declaration and the `cpp.Callable` loader),
   `neko` (CFFI.load) and `hl` (@:hlNative) branches.

2. <lime>/src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx
     + `#if metal import lime.graphics.metal.MetalShaderBridge;`
     + hooks in createShader / shaderSource / attachShader / bindAttribLocation /
       linkProgram / deleteShader / deleteProgram

3. <openfl>/src/openfl/utils/GLSLSourceAssembler.hx
     + `#if metal` branches in getDefaultVersion() / getDefaultLegacyVersion()
       forcing "120", so the Metal path only ever feeds the translator one
       legacy GLSL dialect (macOS would otherwise ask for "410 core").

4. (fallback, only when 3 does not exist) <openfl>/src/openfl/display/Shader.hx
     Older OpenFL revisions assemble the GLSL prefix inside Shader.hx.  In that
     case the shader `#version` literal (or the GLSLSourceAssembler.getDefaultVersion
     call site) is wrapped in an inline `#if metal "120" #else ... #end`.  This hunk
     is best-effort: if none of the known anchors is found it prints a SKIP with the
     reason and leaves the file untouched.

Properties
----------
* Idempotent: every hunk is recognised by a unique `[cne-metal]` marker string,
  so a second run reports "already patched" and writes nothing.
* The mandatory Lime hunks fail loudly: a missing target file or a drifted anchor
  prints the file path, the exact expected anchor text and the surrounding lines,
  and exits non-zero.  The OpenFL version hunks are optional (see
  `--openfl-missing`) and the Shader.hx fallback SKIPs instead of failing.

Usage
-----
    python3 metal/scripts/patch_lime_haxe.py \
        --lime  /path/to/lime \
        --openfl /path/to/openfl

Options:
    --openfl-missing {warn,error,skip}   what to do when
        <openfl>/src/openfl/utils/GLSLSourceAssembler.hx does not exist:
        `warn` (default) prints a SKIP with the reason and still exits 0,
        `error` exits 3, `skip` stays quiet.  In `warn`/`skip` mode the
        Shader.hx fallback patch is attempted.
    --dry-run                            report what would change, write nothing.
"""

import argparse
import difflib
import os
import re
import sys

# ---------------------------------------------------------------------------
# patch table
# ---------------------------------------------------------------------------
#
# Each entry is a dict:
#   id      unique hunk id (used in the report)
#   file    "lime" | "openfl"
#   rel     path below the checkout root
#   marker  unique string that proves the hunk is already in place
#   anchor  exact text that must occur exactly once
#   repl    text the anchor is replaced with
#
# The `#if metal` guard is part of `repl`; the marker comment is immediately
# after it so the same guard can never be inserted twice.

M_CFFI_CPP_DECL = "// [cne-metal] NativeCFFI: cpp declarations"
M_CFFI_CPP_LOAD = "// [cne-metal] NativeCFFI: cpp loaders"
M_CFFI_NEKO = "// [cne-metal] NativeCFFI: neko loaders"
M_CFFI_HL = "// [cne-metal] NativeCFFI: hl natives"
M_CTX_IMPORT = "// [cne-metal] NativeOpenGLRenderContext: bridge import"
M_CTX_CREATE = "// [cne-metal] hook: createShader"
M_CTX_SOURCE = "// [cne-metal] hook: shaderSource"
M_CTX_ATTACH = "// [cne-metal] hook: attachShader"
M_CTX_BINDATTRIB = "// [cne-metal] hook: bindAttribLocation"
M_CTX_LINK = "// [cne-metal] hook: linkProgram"
M_CTX_DELSHADER = "// [cne-metal] hook: deleteShader"
M_CTX_DELPROGRAM = "// [cne-metal] hook: deleteProgram"
M_GLSL_DEFAULT = "// [cne-metal] GLSLSourceAssembler.getDefaultVersion"
M_GLSL_LEGACY = "// [cne-metal] GLSLSourceAssembler.getDefaultLegacyVersion"

PATCHES = [
    # ------------------------------------------------------------------ 1. NativeCFFI
    # cpp: the `@:cffi` declarations (used when `disable_cffi` / haxe < 3.4.0).
    # Modelled on `lime_lzma_decompress(data:Dynamic, bytes:Dynamic):Dynamic`
    # (cpp declaration) and `lime_application_update(handle:Dynamic):Bool`
    # (looked at for the Bool return encoding, which turned out *not* to apply
    # here -- see the note in the inserted comment).
    # NOTE: the `NativeCFFI.hx / cpp @:cffi declarations` hunk that used to live here
    # was removed.  Those declarations sit inside
    #     #if (disable_cffi || haxe_ver < "3.4.0")
    # which is the *fallback* branch and is not compiled for a normal cpp build, so
    # the fields never existed and Haxe reported it as "has no field".  MetalShaderBridge
    # now loads the two primes itself with cpp.Prime._loadPrime, which removes the need
    # to touch Lime's Haxe sources at all.

    # cpp: the cpp.Callable loaders.  "iooo" = Int, Dynamic, Dynamic, value return.
    dict(
        id="NativeCFFI.hx / cpp cpp.Callable loaders",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeCFFI.hx",
        marker=M_CFFI_CPP_LOAD,
        anchor=(
            "\tprivate static var lime_lzma_decompress = new cpp.Callable<cpp.Object->cpp.Object->cpp.Object>(cpp.Prime._loadPrime(\"lime\", \"lime_lzma_decompress\", \"ooo\",\n"
            "\t\tfalse));\n"
            "\tprivate static var lime_mouse_event_manager_register = new cpp.Callable<cpp.Object->cpp.Object->cpp.Void>(cpp.Prime._loadPrime(\"lime\",\n"
        ),
        repl=(
            "\tprivate static var lime_lzma_decompress = new cpp.Callable<cpp.Object->cpp.Object->cpp.Object>(cpp.Prime._loadPrime(\"lime\", \"lime_lzma_decompress\", \"ooo\",\n"
            "\t\tfalse));\n"
            "\t#if metal\n"
            "\t" + M_CFFI_CPP_LOAD + "\n"
            "\t// `lime_lzma_decompress` (previous line) is the model: Dynamic -> cpp.Object -> 'o'.\n"
            "\t// The native `lime_metal_configure_program` is `value (value, value, value)`, hence\n"
            "\t// \"iooo\" and a cpp.Object return (see cffi::CheckSig3 in hxcpp's hx/CFFIPrime.h).\n"
            "\t// `lime_metal_is_metal():Int` mirrors `lime_system_get_num_displays` (\"i\", Void->Int).\n"
            "\tprivate static var lime_metal_configure_program = new cpp.Callable<Int->cpp.Object->cpp.Object->cpp.Object>(cpp.Prime._loadPrime(\"lime\",\n"
            "\t\t\"lime_metal_configure_program\", \"iooo\", false));\n"
            "\tprivate static var lime_metal_is_metal = new cpp.Callable<Void->Int>(cpp.Prime._loadPrime(\"lime\", \"lime_metal_is_metal\", \"i\", false));\n"
            "\t#end\n"
            "\tprivate static var lime_mouse_event_manager_register = new cpp.Callable<cpp.Object->cpp.Object->cpp.Void>(cpp.Prime._loadPrime(\"lime\",\n"
        ),
    ),
    # neko: same name, argument count only (lime.system.CFFI.load).
    dict(
        id="NativeCFFI.hx / neko loaders",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeCFFI.hx",
        marker=M_CFFI_NEKO,
        anchor=(
            "\tprivate static var lime_lzma_decompress = CFFI.load(\"lime\", \"lime_lzma_decompress\", 2);\n"
            "\tprivate static var lime_mouse_event_manager_register = CFFI.load(\"lime\", \"lime_mouse_event_manager_register\", 2);\n"
        ),
        repl=(
            "\tprivate static var lime_lzma_decompress = CFFI.load(\"lime\", \"lime_lzma_decompress\", 2);\n"
            "\t#if metal\n"
            "\t" + M_CFFI_NEKO + "\n"
            "\t// Same two primitives as the cpp branch, loaded by argument count like every other\n"
            "\t// neko primitive.  lime.system.CFFI.load degrades to a reporting stub when the ndll\n"
            "\t// does not export them, so this stays non-fatal on stock Lime builds.\n"
            "\tprivate static var lime_metal_configure_program = CFFI.load(\"lime\", \"lime_metal_configure_program\", 3);\n"
            "\tprivate static var lime_metal_is_metal = CFFI.load(\"lime\", \"lime_metal_is_metal\", 0);\n"
            "\t#end\n"
            "\tprivate static var lime_mouse_event_manager_register = CFFI.load(\"lime\", \"lime_mouse_event_manager_register\", 2);\n"
        ),
    ),
    # hl: @:hlNative, modelled on the hl_* GL entries in the same file.
    dict(
        id="NativeCFFI.hx / hl natives",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeCFFI.hx",
        marker=M_CFFI_HL,
        anchor=(
            "\t@:hlNative(\"lime\", \"hl_lzma_decompress\") private static function lime_lzma_decompress(data:Bytes, bytes:Bytes):Bytes\n"
            "\t{\n"
            "\t\treturn null;\n"
            "\t}\n"
            "\n"
            "\t@:hlNative(\"lime\", \"hl_mouse_event_manager_register\") private static function lime_mouse_event_manager_register(callback:Void->Void,\n"
        ),
        repl=(
            "\t@:hlNative(\"lime\", \"hl_lzma_decompress\") private static function lime_lzma_decompress(data:Bytes, bytes:Bytes):Bytes\n"
            "\t{\n"
            "\t\treturn null;\n"
            "\t}\n"
            "\n"
            "\t#if metal\n"
            "\t" + M_CFFI_HL + "\n"
            "\t// hl counterpart of the two Metal primitives (naming follows the hl_* GL entries).\n"
            "\t// The tables stay `Dynamic` here: MetalShaderBridge passes a Haxe Array, while hl\n"
            "\t// primitives speak varray (cf. `hl_gl_draw_buffers(buffers:hl.NativeArray<Int>)`).\n"
            "\t@:hlNative(\"lime\", \"hl_metal_configure_program\") private static function lime_metal_configure_program(program:Int, attributeMap:Dynamic,\n"
            "\t\tuniformMap:Dynamic):Dynamic\n"
            "\t{\n"
            "\t\treturn null;\n"
            "\t}\n"
            "\n"
            "\t@:hlNative(\"lime\", \"hl_metal_is_metal\") private static function lime_metal_is_metal():Int\n"
            "\t{\n"
            "\t\treturn 0;\n"
            "\t}\n"
            "\t#end\n"
            "\n"
            "\t@:hlNative(\"lime\", \"hl_mouse_event_manager_register\") private static function lime_mouse_event_manager_register(callback:Void->Void,\n"
        ),
    ),
    # ------------------------------------------------- 2. NativeOpenGLRenderContext
    dict(
        id="NativeOpenGLRenderContext.hx / MetalShaderBridge import",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_IMPORT,
        anchor=("import lime.utils.UInt32Array;\n" "\n" "#if !lime_debug\n"),
        repl=(
            "import lime.utils.UInt32Array;\n"
            "#if metal\n"
            + M_CTX_IMPORT + "\n"
            "// The bridge is installed next to lime's own classes by metal/scripts/apply.sh\n"
            "// (src/lime/graphics/metal/MetalShaderBridge.hx).\n"
            "import lime.graphics.metal.MetalShaderBridge;\n"
            "#end\n"
            "\n"
            "#if !lime_debug\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / createShader hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_CREATE,
        anchor=("\t\tvar id = NativeCFFI.lime_gl_create_shader(type);\n" "\t\tif (id == 0) return null;\n"),
        repl=(
            "\t\tvar id = NativeCFFI.lime_gl_create_shader(type);\n"
            "\t\tif (id == 0) return null;\n"
            "\t\t#if metal\n"
            "\t\t" + M_CTX_CREATE + "\n"
            "\t\t// Remember whether this id is a vertex or a fragment shader; shaderSource needs it to\n"
            "\t\t// pick the right half of the MSL translation.\n"
            "\t\tMetalShaderBridge.onCreateShader(id, type);\n"
            "\t\t#end\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / shaderSource hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_SOURCE,
        anchor="\t\tNativeCFFI.lime_gl_shader_source(__getObjectID(shader), source);\n",
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_CTX_SOURCE + "\n"
            "\t\t// Translate the GLSL that OpenFL assembled into MSL *before* it reaches the native\n"
            "\t\t// backend; the backend compiles MSL with newLibraryWithSource.  On a translation\n"
            "\t\t// failure the bridge returns the original GLSL so the error is visible in the log.\n"
            "\t\tsource = MetalShaderBridge.onShaderSource(__getObjectID(shader), source);\n"
            "\t\t#end\n"
            "\t\tNativeCFFI.lime_gl_shader_source(__getObjectID(shader), source);\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / attachShader hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_ATTACH,
        anchor="\t\tNativeCFFI.lime_gl_attach_shader(__getObjectID(program), __getObjectID(shader));\n",
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_CTX_ATTACH + "\n"
            "\t\t// linkProgram must know which shaders belong to a program to collect the\n"
            "\t\t// attribute/uniform tables; the native GL call itself is a no-op under bgfx.\n"
            "\t\tMetalShaderBridge.onAttachShader(__getObjectID(program), __getObjectID(shader));\n"
            "\t\t#end\n"
            "\t\tNativeCFFI.lime_gl_attach_shader(__getObjectID(program), __getObjectID(shader));\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / bindAttribLocation hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_BINDATTRIB,
        anchor="\t\tNativeCFFI.lime_gl_bind_attrib_location(__getObjectID(program), index, name);\n",
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_CTX_BINDATTRIB + "\n"
            "\t\t// OpenFL chooses the GL attribute locations; remember name -> location so\n"
            "\t\t// linkProgram can map them onto the bgfx a_* slots of the translated MSL.\n"
            "\t\tMetalShaderBridge.onBindAttribLocation(__getObjectID(program), index, name);\n"
            "\t\t#end\n"
            "\t\tNativeCFFI.lime_gl_bind_attrib_location(__getObjectID(program), index, name);\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / linkProgram hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_LINK,
        anchor="\t\tNativeCFFI.lime_gl_link_program(__getObjectID(program));\n",
        repl=(
            "\t\tNativeCFFI.lime_gl_link_program(__getObjectID(program));\n"
            "\t\t#if metal\n"
            "\t\t" + M_CTX_LINK + "\n"
            "\t\t// After linking, hand the attribute map (glLocation -> bgfx slot) and the uniform\n"
            "\t\t// descriptor table to the backend through lime_metal_configure_program.\n"
            "\t\tMetalShaderBridge.onLinkProgram(__getObjectID(program));\n"
            "\t\t#end\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / deleteShader hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_DELSHADER,
        anchor="\t\tNativeCFFI.lime_gl_delete_shader(__getObjectID(shader));\n",
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_CTX_DELSHADER + "\n"
            "\t\t// Drop the stage flag and the cached translation for this id.\n"
            "\t\tMetalShaderBridge.onDeleteShader(__getObjectID(shader));\n"
            "\t\t#end\n"
            "\t\tNativeCFFI.lime_gl_delete_shader(__getObjectID(shader));\n"
        ),
    ),
    dict(
        id="NativeOpenGLRenderContext.hx / deleteProgram hook",
        file="lime",
        rel="src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx",
        marker=M_CTX_DELPROGRAM,
        anchor="\t\tNativeCFFI.lime_gl_delete_program(__getObjectID(program));\n",
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_CTX_DELPROGRAM + "\n"
            "\t\t// Drop the per-program attribute/shader bookkeeping.\n"
            "\t\tMetalShaderBridge.onDeleteProgram(__getObjectID(program));\n"
            "\t\t#end\n"
            "\t\tNativeCFFI.lime_gl_delete_program(__getObjectID(program));\n"
        ),
    ),
    # ---------------------------------------------------------- 3. OpenFL GLSL version
    dict(
        id="GLSLSourceAssembler.hx / getDefaultVersion -> 120",
        file="openfl",
        rel="src/openfl/utils/GLSLSourceAssembler.hx",
        marker=M_GLSL_DEFAULT,
        anchor=(
            "\t\t#if (web || mobile)\n"
            "\t\treturn \"300 es\";\n"
        ),
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_GLSL_DEFAULT + "\n"
            "\t\t// The Metal path translates legacy GLSL to MSL (GLSLToMSL), so force the one dialect\n"
            "\t\t// it handles instead of macOS' \"410 core\".\n"
            "\t\treturn \"120\";\n"
            "\t\t#elseif (web || mobile)\n"
            "\t\treturn \"300 es\";\n"
        ),
    ),
    dict(
        id="GLSLSourceAssembler.hx / getDefaultLegacyVersion -> 120",
        file="openfl",
        rel="src/openfl/utils/GLSLSourceAssembler.hx",
        marker=M_GLSL_LEGACY,
        anchor=(
            "\t\t#if (web || mobile || mac)\n"
            "\t\treturn \"100\";\n"
        ),
        repl=(
            "\t\t#if metal\n"
            "\t\t" + M_GLSL_LEGACY + "\n"
            "\t\t// Same reason as getDefaultVersion: macOS would pick \"100\" for legacy shaders, but\n"
            "\t\t// the translator emits a 120-dialect MSL, so keep every Metal shader on \"120\".\n"
            "\t\treturn \"120\";\n"
            "\t\t#elseif (web || mobile || mac)\n"
            "\t\treturn \"100\";\n"
        ),
    ),
]


# ---------------------------------------------------------------------------
# optional fallback: OpenFL revisions that build the GLSL prefix in Shader.hx
# ---------------------------------------------------------------------------
#
# Only attempted when GLSLSourceAssembler.hx does not exist (see main()).  Each
# candidate is tried in order; the first one that matches at least once is used
# and the rest are ignored, so a patched file can never be matched twice.  The
# replacement is an *inline* conditional (`#if metal "120" #else <original> #end`),
# which Haxe accepts anywhere an expression is expected.

M_SHADER_VERSION = "[cne-metal] Shader.hx GLSL version -> 120"

SOFT_PATCHES = [
    dict(
        id="Shader.hx / force legacy GLSL version (fallback)",
        file="openfl",
        rel="src/openfl/display/Shader.hx",
        marker=M_SHADER_VERSION,
        # (label, regex?, pattern, metal-side expression)
        candidates=[
            (
                'a "#version ..." string literal',
                True,
                r'"(?:[^"\\\r\n]|\\.)*#version(?:[^"\\\r\n]|\\.)*"',
                '"#version 120\\n"',
            ),
            (
                'the mac "410 core" string literal',
                True,
                r'"410 core"',
                '"120"',
            ),
            (
                "a GLSLSourceAssembler.getDefaultVersion() call site",
                False,
                "GLSLSourceAssembler.getDefaultVersion()",
                '"120"',
            ),
        ],
    ),
]


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def read_text(path):
    with open(path, "rb") as handle:
        return handle.read().decode("utf-8")


def write_text(path, text):
    with open(path, "wb") as handle:
        handle.write(text.encode("utf-8"))


def first_meaningful_line(text):
    for line in text.splitlines():
        if line.strip():
            return line
    return text


def context_lines(text, needle, radius=8):
    """Return (start_line_no, [lines], note) around the closest match of `needle`."""
    lines = text.splitlines()
    probe = first_meaningful_line(needle)
    index = None
    for i, line in enumerate(lines):
        if line == probe:
            index = i
            break
    note = ""
    if index is None:
        # The anchor has drifted: show the nearest line instead of the top of the
        # file so the failure report is actually actionable.
        close = difflib.get_close_matches(
            probe.strip(), [line.strip() for line in lines], n=1, cutoff=0.5
        )
        if close:
            for i, line in enumerate(lines):
                if line.strip() == close[0]:
                    index = i
                    break
            note = "  (anchor's first line not found; showing the nearest line in the file)"
        else:
            for i, line in enumerate(lines):
                if line.strip():
                    index = i
                    break
            note = "  (anchor's first line not found, and no close match; showing the top of the file)"
    if index is None:
        return 1, ["<file is empty>"], note
    start = max(0, index - radius)
    end = min(len(lines), index + radius + 1)
    return start + 1, lines[start:end], note


def report_anchor_failure(path, patch_id, reason, anchor, source):
    out = []
    out.append("")
    out.append("=" * 78)
    out.append("[FAIL] %s" % patch_id)
    out.append("  file    : %s" % path)
    out.append("  reason  : %s" % reason)
    out.append("  expected anchor (exact text, %d lines):" % len(anchor.splitlines()))
    for line in anchor.splitlines():
        out.append("      | %s" % line)
    start, lines, note = context_lines(source, anchor)
    out.append("  actual context around the expected anchor:")
    if note:
        out.append(note)
    for offset, line in enumerate(lines):
        out.append("      %5d | %s" % (start + offset, line))
    out.append("=" * 78)
    return "\n".join(out)


class Patcher(object):
    def __init__(self, dry_run=False):
        self.dry_run = dry_run
        self.patched = []  # (patch_id, path, ndiff_lines)
        self.up_to_date = []  # (patch_id, path)
        self.failures = []  # strings
        self.skipped = []  # (patch_id, reason)

    def apply(self, patch, root):
        path = os.path.join(root, patch["rel"])
        if not os.path.exists(path):
            self.failures.append(
                report_anchor_failure(
                    path,
                    patch["id"],
                    "target file does not exist below %s" % root,
                    patch["anchor"],
                    "",
                )
            )
            return
        source = read_text(path)

        if patch["marker"] in source:
            self.up_to_date.append((patch["id"], path))
            return

        anchor = patch["anchor"]
        repl = patch["repl"]
        if source.count(anchor) == 0:
            # tolerate CRLF checkouts
            crlf = anchor.replace("\n", "\r\n")
            if source.count(crlf) == 1:
                anchor = crlf
                repl = repl.replace("\n", "\r\n")
            else:
                self.failures.append(
                    report_anchor_failure(
                        path,
                        patch["id"],
                        "anchor not found (found %d times; the checkout has drifted from the "
                        "Lime/OpenFL revision this patcher targets)" % source.count(anchor),
                        patch["anchor"],
                        source,
                    )
                )
                return
        elif source.count(anchor) > 1:
            self.failures.append(
                report_anchor_failure(
                    path,
                    patch["id"],
                    "anchor is ambiguous (%d occurrences)" % source.count(anchor),
                    patch["anchor"],
                    source,
                )
            )
            return

        patched_source = source.replace(anchor, repl, 1)
        added = len(repl.splitlines()) - len(anchor.splitlines())
        if not self.dry_run:
            write_text(path, patched_source)
        self.patched.append((patch["id"], path, added))

    def apply_soft(self, patch, root):
        """Best-effort fallback patch: never fails, SKIPs loudly instead."""
        path = os.path.join(root, patch["rel"])
        if not os.path.exists(path):
            self.skipped.append((patch["id"], "target file does not exist: %s" % path))
            return
        source = read_text(path)

        if patch["marker"] in source:
            self.up_to_date.append((patch["id"], path))
            return

        tried = []
        for label, is_regex, pattern, metal_expr in patch["candidates"]:
            tried.append(label)
            if is_regex:
                matches = re.findall(pattern, source)
            else:
                matches = [pattern] * source.count(pattern)
            if not matches:
                continue

            def wrap(match):
                return "(#if metal %s /* %s */ #else %s #end)" % (metal_expr, patch["marker"], match)

            if is_regex:
                patched_source = re.sub(pattern, lambda m: wrap(m.group(0)), source)
            else:
                patched_source = source.replace(pattern, wrap(pattern))
            if not self.dry_run:
                write_text(path, patched_source)
            self.patched.append((patch["id"] + "  [" + label + " x%d]" % len(matches), path, 0))
            return

        self.skipped.append(
            (
                patch["id"],
                "no known anchor in %s (searched: %s); this OpenFL revision assembles its GLSL "
                "prefix elsewhere, so the GLSLSourceAssembler.hx hook above is the only version patch"
                % (patch["rel"], ", ".join(tried)),
            )
        )


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def normalize_root(path, probe_rel):
    """Accept either a checkout root or its `src` directory."""
    root = os.path.abspath(os.path.expanduser(path))
    if os.path.exists(os.path.join(root, "src", probe_rel)):
        return root
    if os.path.exists(os.path.join(root, probe_rel)):
        return os.path.dirname(root)
    return root


def find_openfl_file(root):
    return os.path.join(root, "src", "openfl", "utils", "GLSLSourceAssembler.hx")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Install the Codename Engine Metal/bgfx Haxe hooks into a Lime checkout.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--lime", required=True, metavar="PATH", help="Lime checkout (or its src/)")
    parser.add_argument("--openfl", default=None, metavar="PATH", help="OpenFL checkout (or its src/)")
    parser.add_argument(
        "--openfl-missing",
        choices=("warn", "error", "skip"),
        default="warn",
        help="what to do when GLSLSourceAssembler.hx is not present (default: warn -> exit 0)",
    )
    parser.add_argument("--dry-run", action="store_true", help="report changes without writing")
    args = parser.parse_args(argv)

    lime_root = normalize_root(args.lime, "lime/_internal/backend/native/NativeCFFI.hx")

    if not os.path.exists(os.path.join(lime_root, "src", "lime/_internal/backend/native/NativeCFFI.hx")):
        sys.stderr.write(
            "error: no Lime checkout at %s\n"
            "       expected %s\n" % (lime_root, os.path.join(lime_root, "src", "lime/_internal/backend/native/NativeCFFI.hx"))
        )
        return 2

    openfl_root = None
    openfl_status = None
    if args.openfl:
        openfl_root = normalize_root(args.openfl, "openfl/utils/GLSLSourceAssembler.hx")
        if not os.path.exists(find_openfl_file(openfl_root)):
            openfl_status = "missing"
            if args.openfl_missing == "error":
                sys.stderr.write(
                    "\n".join(
                        [
                            "=" * 78,
                            "ERROR: OpenFL patch target is missing",
                            "  --openfl : %s" % openfl_root,
                            "  expected : %s" % find_openfl_file(openfl_root),
                            "",
                            "  This checkout has no openfl/utils/GLSLSourceAssembler.hx, which means it",
                            "  predates the class (upstream openfl before 9.5.2 / the cne-openfl branch).",
                            "  The lime patches are still applied, but this GLSL-version hook is mandatory",
                            "  for the Metal path (macOS would ask for \"410 core\").",
                            "",
                            "  Fix by pointing --openfl at a checkout that has the file, e.g.",
                            "    * git -C <checkout> checkout origin/cne      (cne-openfl)",
                            "    * or --openfl /Users/maxwu/haxelib-dev/openfl (== origin/develop)",
                            "  Use --openfl-missing=warn to keep exit 0 and try the Shader.hx fallback.",
                            "=" * 78,
                            "",
                        ]
                    )
                )
            elif args.openfl_missing == "warn":
                sys.stderr.write(
                    "\n".join(
                        [
                            "SKIP: OpenFL GLSLSourceAssembler.hx not present",
                            "  --openfl : %s" % openfl_root,
                            "  missing  : %s" % find_openfl_file(openfl_root),
                            "  This checkout predates openfl/utils/GLSLSourceAssembler.hx, so the two",
                            "  getDefaultVersion()/getDefaultLegacyVersion() hunks are skipped; the optional",
                            "  Shader.hx fallback is attempted instead (it SKIPs when it finds no anchor).",
                            "  (The file exists on origin/cne and origin/develop; the same script patches",
                            "  those checkouts -- just point --openfl at one of them.)",
                            "",
                        ]
                    )
                )

    patcher = Patcher(dry_run=args.dry_run)

    for patch in PATCHES:
        if patch["file"] == "openfl":
            if openfl_root is None or openfl_status == "missing":
                patcher.skipped.append(
                    (
                        patch["id"],
                        "no openfl/utils/GLSLSourceAssembler.hx in this checkout"
                        if openfl_root
                        else "no --openfl given",
                    )
                )
                continue
            patcher.apply(patch, openfl_root)
        else:
            patcher.apply(patch, lime_root)

    # Fallback for OpenFL revisions that assemble the GLSL prefix in Shader.hx
    # (only when GLSLSourceAssembler.hx is absent; never fatal).
    if openfl_root is not None and openfl_status == "missing" and args.openfl_missing != "error":
        for patch in SOFT_PATCHES:
            patcher.apply_soft(patch, openfl_root)
    elif openfl_root is not None and openfl_status is None:
        for patch in SOFT_PATCHES:
            patcher.skipped.append(
                (patch["id"], "not needed: openfl/utils/GLSLSourceAssembler.hx was patched")
            )

    # ------------------------------------------------------------------ report
    verb = "would patch" if args.dry_run else "patched"
    print("")
    print("patch_lime_haxe: Codename Engine Metal/bgfx Haxe hooks")
    print("  lime   : %s" % lime_root)
    print("  openfl : %s" % (openfl_root if openfl_root else "<not given>"))
    print("")
    for patch_id, path, added in patcher.patched:
        if added:
            print("  %-14s %s  (+%d lines)" % (verb, patch_id, added))
        else:
            print("  %-14s %s" % (verb, patch_id))
        print("                 %s" % path)
    for patch_id, path in patcher.up_to_date:
        print("  already done   %s" % patch_id)
    for patch_id, reason in patcher.skipped:
        print("  SKIP           %s  (%s)" % (patch_id, reason))

    for failure in patcher.failures:
        sys.stderr.write(failure + "\n")

    print("")
    print(
        "summary: %d %s, %d already applied, %d skipped, %d failed"
        % (len(patcher.patched), verb, len(patcher.up_to_date), len(patcher.skipped), len(patcher.failures))
    )
    print("")

    if patcher.failures:
        print("FAILED: anchors did not match; nothing was written for the failing hunks.", file=sys.stderr)
        return 1
    if openfl_status == "missing" and args.openfl_missing == "error":
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
