# Metal backend TODO

**English** | [中文](zh-CN/METAL-TODO.md)

**No open actionable items right now.** Everything that can be completed and verified with a
single app build is done: mask (runtime pixel proof), `readPixels` (offscreen immediate +
one-frame backbuffer cache), deterministic pixel-regression goldens, AOT coverage and the
runtime-translation policy decision.

Baseline: **56 / 56** stages ship a prebuilt `.metallib` (the cache covers only the original
engine corpus; mod shaders are translated at runtime) (verified by
`metal/scripts/check-docs.py` and `gen_shader_aot.py --strict`).

## Unfinished optimizations (recorded)

- **D38 draw merging - remaining parts** (the contiguous-index fast path is implemented):
  - merging draws with **non-contiguous index ranges**, or draws in different index buffers,
    requires a merged-index cache (copy the used ranges into a new buffer and key it by the
    range list); not implemented because of cache growth, 16/32-bit index mixing and mod
    geometry risk.
  - **instance-data batching** requires the GLSL->MSL translator and OpenFL to expose
    per-instance attributes; it is an OpenFL batching-layer change, not a backend tweak.
  - The deferred-submit path also means diagnostic paths that record individual draws
    (`cne_capture`, `CNE_METAL_SHOW_WORLDCORNER`) do not see merged draws.

The remaining work is **multi-day architectural / optional** and is tracked in the
[roadmap in README.md](../README.MD#11-future-work-roadmap-not-scheduled): Metal-native
`Context3D`, dropping bgfx, deleting the GL semantics adapter, and 3D/Stage3D support.
