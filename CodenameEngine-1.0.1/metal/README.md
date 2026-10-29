# Metal backend - implementation notes (English index)

**English** | [中文](../../docs/zh-CN/metal/README.md)


The complete, round-by-round development log (5000+ lines: problem -> evidence -> fix) lives in
the Chinese document linked above. This page is the English entry point and maps the log.

## What this backend is

- OpenFL/Lime still call the GL-shaped `lime_gl_*` CFFI; the C++ backend
  (`metal/native/MetalBindings.cpp`) implements every entry point and translates GL state,
  FBOs, textures, uniforms and draws onto **bgfx's Metal renderer**.
- The Lime Haxe hooks (patched by `metal/scripts/patch_lime_haxe.py`) also provide a native
  `cne_metal_*` fast path that skips CFFI and GL enum translation.
- Shaders are translated GLSL -> MSL at build time (`metal/haxe/lime/graphics/metal/GLSLToMSL.hx`)
  and precompiled to `.metallib`; the runtime only performs a hash lookup.

## Map of the Chinese log

| Topic | Where |
| --- | --- |
| Boot/presentation: CAMetalLayer, bgfx swap chain, first pixels | early rounds |
| Orientation/mirroring rules (Y flip decisions) | "direction" rounds |
| Texture byte order (BGRA vs RGBA) and `[cne-tex]` statistics | R/B swap rounds |
| FBO/attachment caching, ping-pong filter passes, extra cameras | FBO rounds |
| Shader AOT pipeline, metallib serialisation, fallbacks | AOT rounds |
| Black-screen root causes (dirty state, RT texture updates, alias sampling) | black-screen rounds |
| Performance work (profiler, index-buffer hashing, batching) | performance rounds |
| Universal build + DMG pipeline | release rounds |
| Known unsupported states and decisions | final rounds |

## Current entry points

- build/run: `README.MD` (repository root)
- diagnostics: `docs/DIAGNOSTICS.md`
- open TODO: `docs/METAL-TODO.md`
- audits: `metal/audit/*.md` (English indexes; Chinese full text under `docs/zh-CN/metal/audit/`)
- verification: `./verify.sh`
