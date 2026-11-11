# Metal backend TODO

**English** | [中文](zh-CN/METAL-TODO.md)

**No open actionable items right now.** Everything that can be completed and verified with a
single app build is done: mask (runtime pixel proof), `readPixels` (offscreen immediate +
one-frame backbuffer cache), deterministic pixel-regression goldens, AOT coverage and the
runtime-translation policy decision.

Baseline: **59 / 59** stages ship a prebuilt `.metallib` (the cache covers only the original
engine corpus; mod shaders are translated at runtime) (verified by
`metal/scripts/check-docs.py` and `gen_shader_aot.py --strict`).



## Unfinished optimizations (recorded)

- **D38 draw merging - remaining part**: contiguous *and* non-contiguous index ranges (same
  or different index buffers) are merged through the merged-index cache (bounded to 64
  ranges and 3M indices per draw; cache capped at 128 entries / 8 MB).  Diagnostics that
  record individual draws (`cne_capture`, `CNE_METAL_SHOW_WORLDCORNER`) do not see merged
  draws.  Backend-side instancing support is now in place: GLSL `gl_InstanceID` and
  `gl_VertexID` translate to Metal's `[[instance_id]]` / `[[vertex_id]]`, and
  `glDraw*Instanced` already forwards the instance count.  True per-instance-attribute
  batching still needs OpenFL/Flixel to emit instance data from its batching layer.

The remaining work is **multi-day architectural / optional** and is tracked in the
[roadmap in README.md](../README.MD#11-future-work-roadmap-not-scheduled): Metal-native
`Context3D`, dropping bgfx, deleting the GL semantics adapter, and 3D/Stage3D support.
