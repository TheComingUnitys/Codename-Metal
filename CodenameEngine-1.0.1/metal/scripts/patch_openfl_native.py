#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
patch_openfl_native.py -- installation of the Phase-1 "native Metal 2D path"
hooks into the OpenFL checkout.

The native path is a *parallel* implementation: it is only taken when
`CNE_METAL_NATIVE_PATH=<stages>` is set at runtime, and every hook keeps the
original GL call in the `else` branch, so the default (env unset) path is
byte-for-byte the old one.

Phase 1a covers the **state stage**: viewport, scissor, blend enable/factors/
equation, colour mask, depth mask/test, culling enable, front face, clear and
the clear colour/depth/stencil values.  See `metal/audit/openfl-native.md`.

Patch targets:
  <openfl>/src/openfl/display3D/Context3D.hx
  <openfl>/src/openfl/display/OpenGLRenderer.hx

Properties: idempotent (`[cne-native]` markers), fails loudly on drifted
anchors, `--dry-run` reports only.

Usage:
    python3 metal/scripts/patch_openfl_native.py --openfl <path-to-openfl>
"""

import argparse
import os
import sys

M_IMPORT = "// [cne-native] import"
M_VIEWPORT = "// [cne-native] viewport"
M_SCISSOR = "// [cne-native] scissor"
M_STATE = "// [cne-native] state"


def wrap(prefix_lines, native_stmt, gl_stmt, marker):
    """#if metal native else #end gl_stmt (single-statement form)."""
    indent = "".join("\t" for _ in [0])  # callers embed their own indentation
    native_lines = "".join("\t\t\t\t" + line + "\n" for line in prefix_lines)
    return (
        "\t\t\t\t#if metal\n"
        + native_lines
        + "\t\t\t\telse\n"
        + "\t\t\t\t#end\n"
        + gl_stmt
    )


PATCHES = [
    # ------------------------------------------------------------- Context3D
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-import",
        marker=M_IMPORT,
        count=1,
        anchor="import openfl.utils.ByteArray;\n",
        repl=(
            "import openfl.utils.ByteArray;\n"
            "#if metal\n"
            "import lime.graphics.metal.MetalNative; " + M_IMPORT + "\n"
            "#end\n"
        ),
    ),
    # __flushGLViewport: 3 gl.viewport call sites
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-viewport-stage3d",
        marker="// [cne-native] viewport scaled",
        count=1,
        anchor="\t\t\t\tgl.viewport(x, y, scaledBackBufferWidth, scaledBackBufferHeight);\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setViewport(x, y, scaledBackBufferWidth, scaledBackBufferHeight); " + M_VIEWPORT + " scaled\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.viewport(x, y, scaledBackBufferWidth, scaledBackBufferHeight);\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-viewport-backbuffer",
        marker="// [cne-native] viewport backbuffer",
        count=1,
        anchor="\t\t\t\tgl.viewport(0, 0, backBufferWidth, backBufferHeight);\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setViewport(0, 0, backBufferWidth, backBufferHeight); " + M_VIEWPORT + " backbuffer\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.viewport(0, 0, backBufferWidth, backBufferHeight);\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-viewport-rtt",
        marker="// [cne-native] viewport rtt",
        count=1,
        anchor="\t\t\tgl.viewport(0, 0, width, height);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setViewport(0, 0, width, height); " + M_VIEWPORT + " rtt\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.viewport(0, 0, width, height);\n"
        ),
    ),
    # __flushGLScissor
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-scissor-rect",
        marker="// [cne-native] scissor rect",
        count=1,
        anchor="\t\t\t\tgl.scissor(scissorX, scissorY, scissorWidth, scissorHeight);\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setScissorRect(scissorX, scissorY, scissorWidth, scissorHeight); " + M_SCISSOR + " rect\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.scissor(scissorX, scissorY, scissorWidth, scissorHeight);\n"
        ),
    ),
]

# The enable/disable toggles share their shape; generated programmatically for the
# four caps the native state stage owns.
for cap, marker_id, native in [
    ("BLEND", "ctx-toggle-blend", "setBlend"),
    ("SCISSOR_TEST", "ctx-toggle-scissor", "setScissorTest"),
    ("DEPTH_TEST", "ctx-toggle-depth", "setDepthTestEnabled"),
    ("CULL_FACE", "ctx-toggle-cull", "setCullEnabled"),
]:
    anchor = (
        "\t\t\tif (enable)\n"
        "\t\t\t{\n"
        "\t\t\t\tgl.enable(gl." + cap + ");\n"
        "\t\t\t}\n"
        "\t\t\telse\n"
        "\t\t\t{\n"
        "\t\t\t\tgl.disable(gl." + cap + ");\n"
        "\t\t\t}\n"
    )
    repl = (
        "\t\t\t#if metal\n"
        "\t\t\tif (MetalNative.active(\"state\"))\n"
        "\t\t\t{\n"
        "\t\t\t\tMetalNative." + native + "(enable); " + M_STATE + " " + cap + "\n"
        "\t\t\t}\n"
        "\t\t\telse\n"
        "\t\t\t#end\n"
        + anchor
    )
    PATCHES.append(
        dict(
            file="src/openfl/display3D/Context3D.hx",
            id=marker_id,
            marker="// [cne-native] state " + cap,
            count=1,
            anchor=anchor,
            repl=repl,
        )
    )

PATCHES += [
    # blend equation
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-blend-equation",
        marker="// [cne-native] blend equation",
        count=1,
        anchor="\t\t\tgl.blendEquation(value);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setBlendEquationFromGL(value); // [cne-native] blend equation\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.blendEquation(value);\n"
        ),
    ),
    # blend functions (same factors / separate factors)
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-blendfunc",
        marker="// [cne-native] blendfunc",
        count=1,
        anchor="\t\t\t\tgl.blendFunc(__getGLBlend(__state.blendSourceRGBFactor), __getGLBlend(__state.blendDestinationRGBFactor));\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setBlendFactors(__state.blendSourceRGBFactor, __state.blendDestinationRGBFactor, __state.blendSourceRGBFactor, __state.blendDestinationRGBFactor); // [cne-native] blendfunc\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.blendFunc(__getGLBlend(__state.blendSourceRGBFactor), __getGLBlend(__state.blendDestinationRGBFactor));\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-blendfunc-separate",
        marker="// [cne-native] blendfunc separate",
        count=1,
        anchor="\t\t\t\tgl.blendFuncSeparate(__getGLBlend(__state.blendSourceRGBFactor), __getGLBlend(__state.blendDestinationRGBFactor),\n\t\t\t\t\t__getGLBlend(__state.blendSourceAlphaFactor), __getGLBlend(__state.blendDestinationAlphaFactor));\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setBlendFactors(__state.blendSourceRGBFactor, __state.blendDestinationRGBFactor, __state.blendSourceAlphaFactor, __state.blendDestinationAlphaFactor); // [cne-native] blendfunc separate\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.blendFuncSeparate(__getGLBlend(__state.blendSourceRGBFactor), __getGLBlend(__state.blendDestinationRGBFactor),\n"
            "\t\t\t\t\t__getGLBlend(__state.blendSourceAlphaFactor), __getGLBlend(__state.blendDestinationAlphaFactor));\n"
        ),
    ),
    # colour mask (flush)
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-colormask",
        marker="// [cne-native] colormask",
        count=1,
        anchor="\t\t\tgl.colorMask(__state.colorMaskRed, __state.colorMaskGreen, __state.colorMaskBlue, __state.colorMaskAlpha);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setColorMask(__state.colorMaskRed, __state.colorMaskGreen, __state.colorMaskBlue, __state.colorMaskAlpha); // [cne-native] colormask\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.colorMask(__state.colorMaskRed, __state.colorMaskGreen, __state.colorMaskBlue, __state.colorMaskAlpha);\n"
        ),
    ),
    # depth mask (flush)
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-depthmask",
        marker="// [cne-native] depthmask",
        count=1,
        anchor="\t\t\tgl.depthMask(depthMask);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setDepthWrite(depthMask); // [cne-native] depthmask\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.depthMask(depthMask);\n"
        ),
    ),
    # front face
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="ctx-frontface",
        marker="// [cne-native] frontface",
        count=1,
        anchor="\t\t\tgl.frontFace(counterClockWise ? gl.CCW : gl.CW);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setFrontFace(counterClockWise); // [cne-native] frontface\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.frontFace(counterClockWise ? gl.CCW : gl.CW);\n"
        ),
    ),
    # ------------------------------------------------------------- __clear
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="clear-colormask",
        marker="// [cne-native] clear colormask",
        count=1,
        anchor="\t\t\t\tgl.colorMask(true, true, true, true);\n\t\t\t\t__contextState.colorMaskRed = true;\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setColorMask(true, true, true, true); // [cne-native] clear colormask\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.colorMask(true, true, true, true);\n"
            "\t\t\t\t__contextState.colorMaskRed = true;\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="clear-color",
        marker="// [cne-native] clear-color-value",
        count=1,
        anchor="\t\t\tgl.clearColor(red, green, blue, alpha);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setClearColor(red, green, blue, alpha); // [cne-native] clear-color-value\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.clearColor(red, green, blue, alpha);\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="clear-depthmask",
        marker="// [cne-native] clear depthmask",
        count=1,
        anchor="\t\t\t\tgl.depthMask(true);\n\t\t\t\t__contextState.depthMask = true;\n",
        repl=(
            "\t\t\t\t#if metal\n"
            "\t\t\t\tif (MetalNative.active(\"state\")) MetalNative.setDepthWrite(true); // [cne-native] clear depthmask\n"
            "\t\t\t\telse\n"
            "\t\t\t\t#end\n"
            "\t\t\t\tgl.depthMask(true);\n"
            "\t\t\t\t__contextState.depthMask = true;\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="clear-depth",
        marker="// [cne-native] clear-depth-value",
        count=1,
        anchor="\t\t\tgl.clearDepth(depth);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setClearDepth(depth); // [cne-native] clear-depth-value\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.clearDepth(depth);\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="clear-stencil",
        marker="// [cne-native] clear stencil",
        count=1,
        anchor="\t\t\tgl.clearStencil(stencil);\n",
        repl=(
            "\t\t\t#if metal\n"
            "\t\t\tif (MetalNative.active(\"state\")) MetalNative.setClearStencil(stencil); // [cne-native] clear stencil\n"
            "\t\t\telse\n"
            "\t\t\t#end\n"
            "\t\t\tgl.clearStencil(stencil);\n"
        ),
    ),
    dict(
        file="src/openfl/display3D/Context3D.hx",
        id="clear-clear",
        marker="// [cne-native] clear-bits",
        count=1,
        anchor="\t\tgl.clear(clearMask);\n",
        repl=(
            "\t\t#if metal\n"
            "\t\tif (MetalNative.active(\"state\")) MetalNative.clear((clearMask & gl.COLOR_BUFFER_BIT) != 0, (clearMask & gl.DEPTH_BUFFER_BIT) != 0, (clearMask & gl.STENCIL_BUFFER_BIT) != 0); // [cne-native] clear-bits\n"
            "\t\telse\n"
            "\t\t#end\n"
            "\t\tgl.clear(clearMask);\n"
        ),
    ),
    # ------------------------------------------------------- OpenGLRenderer
    dict(
        file="src/openfl/display/OpenGLRenderer.hx",
        id="renderer-import",
        marker="// [cne-native] renderer import",
        count=1,
        anchor="import openfl.geom.Rectangle;\n",
        repl=(
            "import openfl.geom.Rectangle;\n"
            "#if metal\n"
            "import lime.graphics.metal.MetalNative; // [cne-native] renderer import\n"
            "#end\n"
        ),
    ),
    dict(
        file="src/openfl/display/OpenGLRenderer.hx",
        id="renderer-viewport",
        marker="// [cne-native] renderer viewport",
        count=1,
        anchor="\t\t__gl.viewport(__offsetX, __offsetY, __displayWidth, __displayHeight);\n",
        repl=(
            "\t\t#if metal\n"
            "\t\tif (MetalNative.active(\"state\")) MetalNative.setViewport(__offsetX, __offsetY, __displayWidth, __displayHeight); // [cne-native] renderer viewport\n"
            "\t\telse\n"
            "\t\t#end\n"
            "\t\t__gl.viewport(__offsetX, __offsetY, __displayWidth, __displayHeight);\n"
        ),
    ),
    dict(
        file="src/openfl/display/OpenGLRenderer.hx",
        id="renderer-letterbox-clear",
        marker="// [cne-native] renderer letterbox clear",
        count=4,
        anchor="\t\t\t\t\t__gl.clearColor(0, 0, 0, 1);\n\t\t\t\t\t__gl.clear(__gl.COLOR_BUFFER_BIT);\n",
        repl=(
            "\t\t\t\t\t#if metal\n"
            "\t\t\t\t\tif (MetalNative.active(\"state\"))\n"
            "\t\t\t\t\t{\n"
            "\t\t\t\t\t\tMetalNative.setClearColor(0, 0, 0, 1);\n"
            "\t\t\t\t\t\tMetalNative.clear(true, false, false); // [cne-native] renderer letterbox clear\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t\telse\n"
            "\t\t\t\t\t{\n"
            "\t\t\t\t\t#end\n"
            "\t\t\t\t\t__gl.clearColor(0, 0, 0, 1);\n"
            "\t\t\t\t\t__gl.clear(__gl.COLOR_BUFFER_BIT);\n"
            "\t\t\t\t\t#if metal\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t\t#end\n"
        ),
    ),
]


def apply_patches(openfl_dir, dry_run):
    patched, already, failed = [], [], []
    for p in PATCHES:
        path = os.path.join(openfl_dir, p["file"])
        if not os.path.isfile(path):
            failed.append((p["id"], path, "file not found"))
            continue
        src = open(path).read()
        if p["marker"] in src:
            already.append(p["id"])
            continue
        count = src.count(p["anchor"])
        if count != p["count"]:
            failed.append((p["id"], path, "anchor found %d time(s), expected %d" % (count, p["count"])))
            continue
        if dry_run:
            patched.append(p["id"])
            continue
        src = src.replace(p["anchor"], p["repl"])
        open(path, "w").write(src)
        patched.append(p["id"])
    return patched, already, failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--openfl", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--stage", choices=["all", "state", "draw"], default="all")
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(args.openfl, "src", "openfl")):
        sys.stderr.write("error: %s does not look like an OpenFL checkout\n" % args.openfl)
        return 2

    rc = 0

    if args.stage in ("all", "state"):
        print("openfl : %s  (state stage)" % args.openfl)
        patched, already, failed = apply_patches(args.openfl, args.dry_run)
        verb = "WOULD PATCH" if args.dry_run else "patched"
        for pid in patched:
            print("  %-12s %s" % (verb, pid))
        for pid in already:
            print("  already done %s" % pid)
        for pid, path, why in failed:
            print("  FAILED       %-28s %s (%s)" % (pid, why, path))
        print("summary: %d patched, %d already applied, %d failed"
              % (len(patched), len(already), len(failed)))
        if failed:
            rc = 1

    if args.stage in ("all", "draw"):
        if main_draw(args.openfl, args.dry_run) != 0:
            rc = 1

    return rc


# ============================================================================
# Phase 1 / draw stage: rewrite the per-draw GL calls to the explicit native
# wrappers (MetalNative.*).  Each wrapper branches internally: native when the
# "draw" stage is enabled, otherwise it performs exactly the old GL call, so the
# rewrite is behaviour-neutral with CNE_METAL_NATIVE_PATH unset.
#
# These are simple prefix rewrites; the pattern disappearing from the file is the
# idempotency marker.
# ============================================================================

DRAW_IMPORT = "#if metal\nimport lime.graphics.metal.MetalNative; // [cne-native] draw import\n#end\n"

DRAW_PATCHES = [
    # --------------------------------------------------------------- ShaderParameter
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-import",
         anchor="import openfl.display3D.Context3D;\n",
         anchor_repl="import openfl.display3D.Context3D;\n" + DRAW_IMPORT),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-disable-attrib",
         needle="gl.disableVertexAttribArray(", repl="MetalNative.disableAttrib(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform-m2",
         needle="gl.uniformMatrix2fv(", repl="MetalNative.uniformMatrix2fv(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform-m3",
         needle="gl.uniformMatrix3fv(", repl="MetalNative.uniformMatrix3fv(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform-m4",
         needle="gl.uniformMatrix4fv(", repl="MetalNative.uniformMatrix4fv(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform1i",
         needle="gl.uniform1i(", repl="MetalNative.uniform1i(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform2i",
         needle="gl.uniform2i(", repl="MetalNative.uniform2i(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform3i",
         needle="gl.uniform3i(", repl="MetalNative.uniform3i(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform4i",
         needle="gl.uniform4i(", repl="MetalNative.uniform4i(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform1f",
         needle="gl.uniform1f(", repl="MetalNative.uniform1f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform2f",
         needle="gl.uniform2f(", repl="MetalNative.uniform2f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform3f",
         needle="gl.uniform3f(", repl="MetalNative.uniform3f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-uniform4f",
         needle="gl.uniform4f(", repl="MetalNative.uniform4f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-attrib1f",
         needle="gl.vertexAttrib1f(", repl="MetalNative.attribConstant1f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-attrib2f",
         needle="gl.vertexAttrib2f(", repl="MetalNative.attribConstant2f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-attrib3f",
         needle="gl.vertexAttrib3f(", repl="MetalNative.attribConstant3f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-attrib4f",
         needle="gl.vertexAttrib4f(", repl="MetalNative.attribConstant4f(gl, "),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-enable-attrib",
         needle="gl.enableVertexAttribArray(index + i);", repl="MetalNative.enableAttrib(gl, index + i);"),
    dict(file="src/openfl/display/ShaderParameter.hx", id="sp-vertex-attrib-pointer",
         needle="gl.vertexAttribPointer(index + i, __length, type, false, __length * Float32Array.BYTES_PER_ELEMENT,",
         repl="MetalNative.vertexAttribCurrent(gl, index + i, __length, type, false, __length * Float32Array.BYTES_PER_ELEMENT,"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-enable-blend",
         needle="gl.enable(gl.BLEND);", repl="MetalNative.setCap(gl, gl.BLEND, true);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-disable-blend",
         needle="gl.disable(gl.BLEND);", repl="MetalNative.setCap(gl, gl.BLEND, false);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-enable-cull_face",
         needle="gl.enable(gl.CULL_FACE);", repl="MetalNative.setCap(gl, gl.CULL_FACE, true);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-disable-cull_face",
         needle="gl.disable(gl.CULL_FACE);", repl="MetalNative.setCap(gl, gl.CULL_FACE, false);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-enable-depth_test",
         needle="gl.enable(gl.DEPTH_TEST);", repl="MetalNative.setCap(gl, gl.DEPTH_TEST, true);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-disable-depth_test",
         needle="gl.disable(gl.DEPTH_TEST);", repl="MetalNative.setCap(gl, gl.DEPTH_TEST, false);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-enable-scissor_test",
         needle="gl.enable(gl.SCISSOR_TEST);", repl="MetalNative.setCap(gl, gl.SCISSOR_TEST, true);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-disable-scissor_test",
         needle="gl.disable(gl.SCISSOR_TEST);", repl="MetalNative.setCap(gl, gl.SCISSOR_TEST, false);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-enable-stencil_test",
         needle="gl.enable(gl.STENCIL_TEST);", repl="MetalNative.setCap(gl, gl.STENCIL_TEST, true);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-disable-stencil_test",
         needle="gl.disable(gl.STENCIL_TEST);", repl="MetalNative.setCap(gl, gl.STENCIL_TEST, false);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-enable-texture2d-context3d",
         needle="gl.enable(gl.TEXTURE_2D);", repl="MetalNative.setCap(gl, gl.TEXTURE_2D, true);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="cap-disable-texture2d-context3d",
         needle="gl.disable(gl.TEXTURE_2D);", repl="MetalNative.setCap(gl, gl.TEXTURE_2D, false);"),
    dict(file="src/openfl/display/Shader.hx", id="cap-enable-texture2d-shader",
         needle="gl.enable(gl.TEXTURE_2D);", repl="MetalNative.setCap(gl, gl.TEXTURE_2D, true);"),
    dict(file="src/openfl/display/Shader.hx", id="cap-disable-texture2d-shader",
         needle="gl.disable(gl.TEXTURE_2D);", repl="MetalNative.setCap(gl, gl.TEXTURE_2D, false);"),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="tex-upload-9",
         needle="gl.texImage2D(gl.TEXTURE_2D, 0, internalFormat, image.buffer.width, image.buffer.height, 0, format, gl.UNSIGNED_BYTE, image.data);",
         repl="MetalNative.texImage2D(gl, gl.TEXTURE_2D, 0, internalFormat, image.buffer.width, image.buffer.height, 0, format, gl.UNSIGNED_BYTE, image.data);"),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="tex-param-min",
         needle="gl.texParameteri(__textureTarget, gl.TEXTURE_MIN_FILTER, minFilter);",
         repl="MetalNative.texParameteri(gl, __textureTarget, gl.TEXTURE_MIN_FILTER, minFilter);"),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="texturebase-import",
         anchor="import openfl.display3D._internal.GLTexture;\n",
         anchor_repl="import openfl.display3D._internal.GLTexture;\n" + DRAW_IMPORT),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="tex-param-mag",
         needle="gl.texParameteri(__textureTarget, gl.TEXTURE_MAG_FILTER, magFilter);",
         repl="MetalNative.texParameteri(gl, __textureTarget, gl.TEXTURE_MAG_FILTER, magFilter);"),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="tex-param-wraps",
         needle="gl.texParameteri(__textureTarget, gl.TEXTURE_WRAP_S, wrapModeS);",
         repl="MetalNative.texParameteri(gl, __textureTarget, gl.TEXTURE_WRAP_S, wrapModeS);"),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="tex-param-wrapt",
         needle="gl.texParameteri(__textureTarget, gl.TEXTURE_WRAP_T, wrapModeT);",
         repl="MetalNative.texParameteri(gl, __textureTarget, gl.TEXTURE_WRAP_T, wrapModeT);"),
    dict(file="src/openfl/display3D/VertexBuffer3D.hx", id="obj-create-buffer",
         needle="gl.createBuffer()", repl="MetalNative.createBuffer(gl)"),
    dict(file="src/openfl/display3D/IndexBuffer3D.hx", id="obj-create-buffer-ib",
         needle="gl.createBuffer()", repl="MetalNative.createBuffer(gl)"),
    dict(file="src/openfl/display/Shader.hx", id="obj-create-buffer-shader",
         needle="gl.createBuffer()", repl="MetalNative.createBuffer(gl)"),
    dict(file="src/openfl/display3D/textures/TextureBase.hx", id="obj-create-texture",
         needle="gl.createTexture()", repl="MetalNative.createTexture(gl)"),
    dict(file="src/openfl/display3D/Program3D.hx", id="obj-get-uniform",
         needle="gl.getUniformLocation(", repl="MetalNative.getUniformLocation(gl, "),
    dict(file="src/openfl/display3D/Program3D.hx", id="obj-get-attrib",
         needle="gl.getAttribLocation(", repl="MetalNative.getAttribLocation(gl, "),
    dict(file="src/openfl/display/Shader.hx", id="obj-get-uniform-shader",
         needle="gl.getUniformLocation(", repl="MetalNative.getUniformLocation(gl, "),
    dict(file="src/openfl/display/Shader.hx", id="obj-get-attrib-shader",
         needle="gl.getAttribLocation(", repl="MetalNative.getAttribLocation(gl, "),
    dict(file="src/openfl/display3D/Program3D.hx", id="obj-bind-attrib",
         needle="gl.bindAttribLocation(", repl="MetalNative.bindAttribLocation(gl, "),
    dict(file="src/openfl/display/Shader.hx", id="obj-bind-attrib-shader",
         needle="gl.bindAttribLocation(", repl="MetalNative.bindAttribLocation(gl, "),
    # --------------------------------------------------------------- uploads
    dict(file="src/openfl/display3D/VertexBuffer3D.hx", id="vb-import",
         anchor="import openfl.display3D._internal.GLBuffer;\n",
         anchor_repl="import openfl.display3D._internal.GLBuffer;\n" + DRAW_IMPORT),
    dict(file="src/openfl/display3D/VertexBuffer3D.hx", id="vb-upload",
         needle="gl.bufferData(gl.ARRAY_BUFFER, data, __usage);",
         repl="MetalNative.bufferData(gl, gl.ARRAY_BUFFER, data, __usage);"),
    dict(file="src/openfl/display3D/IndexBuffer3D.hx", id="ib-import",
         anchor="import openfl.display3D._internal.GLBuffer;\n",
         anchor_repl="import openfl.display3D._internal.GLBuffer;\n" + DRAW_IMPORT),
    dict(file="src/openfl/display3D/IndexBuffer3D.hx", id="ib-upload",
         needle="gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, data, __usage);",
         repl="MetalNative.bufferData(gl, gl.ELEMENT_ARRAY_BUFFER, data, __usage);"),
    dict(file="src/openfl/display/Shader.hx", id="shader-paramdata-upload",
         needle="gl.bufferData(gl.ARRAY_BUFFER, shaderBuffer.paramData, gl.DYNAMIC_DRAW);",
         repl="MetalNative.bufferData(gl, gl.ARRAY_BUFFER, shaderBuffer.paramData, gl.DYNAMIC_DRAW);"),
    # --------------------------------------------------------------- Shader
    dict(file="src/openfl/display/Shader.hx", id="shader-import",
         anchor="import openfl.display3D.Context3D;\n",
         anchor_repl="import openfl.display3D.Context3D;\n" + DRAW_IMPORT),
    dict(file="src/openfl/display/Shader.hx", id="shader-uniform1i",
         needle="gl.uniform1i(", repl="MetalNative.uniform1i(gl, "),
    # --------------------------------------------------------------- Program3D
    dict(file="src/openfl/display3D/Program3D.hx", id="prog-import",
         anchor="import openfl.display3D._internal.GLProgram;\n",
         anchor_repl="import openfl.display3D._internal.GLProgram;\n" + DRAW_IMPORT),
    dict(file="src/openfl/display3D/Program3D.hx", id="prog-use",
         needle="gl.useProgram(__glProgram);", repl="MetalNative.useProgram(gl, __glProgram);"),
    # --------------------------------------------------------------- Context3D
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-bind-array",
         needle="gl.bindBuffer(gl.ARRAY_BUFFER, buffer);", repl="MetalNative.bindArrayBuffer(gl, buffer);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-bind-element",
         needle="gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, buffer);", repl="MetalNative.bindElementBuffer(gl, buffer);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-enable-attrib",
         needle="gl.enableVertexAttribArray(", repl="MetalNative.enableAttrib(gl, "),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-disable-attrib",
         needle="gl.disableVertexAttribArray(", repl="MetalNative.disableAttrib(gl, "),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-attrib-ubyte4",
         needle="gl.vertexAttribPointer(index, 4, gl.UNSIGNED_BYTE, true, buffer.__stride, byteOffset);",
         repl="MetalNative.vertexAttrib(gl, index, buffer.__id, 4, MetalNative.ATTRIB_UNSIGNED_BYTE, true, buffer.__stride, byteOffset);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-attrib-f4",
         needle="gl.vertexAttribPointer(index, 4, gl.FLOAT, false, buffer.__stride, byteOffset);",
         repl="MetalNative.vertexAttrib(gl, index, buffer.__id, 4, MetalNative.ATTRIB_FLOAT, false, buffer.__stride, byteOffset);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-attrib-f3",
         needle="gl.vertexAttribPointer(index, 3, gl.FLOAT, false, buffer.__stride, byteOffset);",
         repl="MetalNative.vertexAttrib(gl, index, buffer.__id, 3, MetalNative.ATTRIB_FLOAT, false, buffer.__stride, byteOffset);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-attrib-f2",
         needle="gl.vertexAttribPointer(index, 2, gl.FLOAT, false, buffer.__stride, byteOffset);",
         repl="MetalNative.vertexAttrib(gl, index, buffer.__id, 2, MetalNative.ATTRIB_FLOAT, false, buffer.__stride, byteOffset);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-attrib-f1",
         needle="gl.vertexAttribPointer(index, 1, gl.FLOAT, false, buffer.__stride, byteOffset);",
         repl="MetalNative.vertexAttrib(gl, index, buffer.__id, 1, MetalNative.ATTRIB_FLOAT, false, buffer.__stride, byteOffset);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-active-texture",
         needle="gl.activeTexture(gl.TEXTURE0 + sampler);", repl="MetalNative.activeTexture(gl, sampler);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-active-texture4",
         needle="gl.activeTexture(gl.TEXTURE0 + sampler + 4);", repl="MetalNative.activeTexture(gl, sampler + 4);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-bind-texture",
         needle="gl.bindTexture(gl.TEXTURE_2D, texture);", repl="MetalNative.bindTexture2D(gl, texture);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-draw-elements",
         needle="gl.drawElements(gl.TRIANGLES, count, gl.UNSIGNED_SHORT, firstIndex * 2);",
         repl="MetalNative.drawIndexed(gl, MetalNative.PRIMITIVE_TRIANGLES, count, MetalNative.INDEX_UNSIGNED_SHORT, firstIndex * 2);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-draw-arrays",
         needle="gl.drawArrays(gl.TRIANGLES, firstIndex, count);",
         repl="MetalNative.drawArrays(gl, MetalNative.PRIMITIVE_TRIANGLES, firstIndex, count);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-uniform-m4",
         needle="gl.uniformMatrix4fv(cast firstRegister, transposedMatrix, data);",
         repl="MetalNative.uniformMatrix4fv(gl, cast firstRegister, transposedMatrix, data);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-alpha-sampler-on",
         needle="gl.uniform1i(__state.program.__agalAlphaSamplerEnabled[sampler].location, 1);",
         repl="MetalNative.uniform1i(gl, __state.program.__agalAlphaSamplerEnabled[sampler].location, 1);"),
    dict(file="src/openfl/display3D/Context3D.hx", id="ctx-alpha-sampler-off",
         needle="gl.uniform1i(__state.program.__agalAlphaSamplerEnabled[sampler].location, 0);",
         repl="MetalNative.uniform1i(gl, __state.program.__agalAlphaSamplerEnabled[sampler].location, 0);"),
]


def apply_draw_patches(openfl_dir, dry_run):
    patched, already, failed = [], [], []
    for p in DRAW_PATCHES:
        path = os.path.join(openfl_dir, p["file"])
        if not os.path.isfile(path):
            failed.append((p["id"], path, "file not found")); continue
        src = open(path).read()
        if "anchor" in p:
            if p["anchor_repl"] in src:
                already.append(p["id"]); continue
            if src.count(p["anchor"]) != 1:
                failed.append((p["id"], path, "anchor count %d" % src.count(p["anchor"]))); continue
            if not dry_run:
                open(path, "w").write(src.replace(p["anchor"], p["anchor_repl"], 1))
            patched.append(p["id"]); continue
        count = src.count(p["needle"])
        if count == 0:
            already.append(p["id"]); continue
        if not dry_run:
            open(path, "w").write(src.replace(p["needle"], p["repl"]))
        patched.append(p["id"])
    return patched, already, failed


def main_draw(openfl_dir, dry_run):
    print("openfl : %s  (draw stage)" % openfl_dir)
    patched, already, failed = apply_draw_patches(openfl_dir, dry_run)
    for pid in patched:
        print("  %-12s %s" % ("WOULD REWRITE" if dry_run else "rewritten", pid))
    for pid in already:
        print("  already done %s" % pid)
    for pid, path, why in failed:
        print("  FAILED       %-28s %s (%s)" % (pid, why, path))
    print("summary: %d rewritten, %d already applied, %d failed" % (len(patched), len(already), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
