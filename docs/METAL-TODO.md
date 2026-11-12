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

- **D38**: complete.  Contiguous and non-contiguous index ranges are merged through the
  merged-index cache; **instanced bitmap batching** is implemented (opt-in: it skips the
  offscreen late matrix flip, so filtered stages rotate; enable with CNE_METAL_INSTANCING=1)
  in the
  [openfl-metal](https://github.com/TheComingUnitys/openfl-metal) and
  [flixel-metal](https://github.com/TheComingUnitys/flixel-metal) forks
  (`InstancedDisplayObjectShader` + `MetalInstanceBatch`, `FlxG.metalInstancing`).  The
  native backend maps `i_data0..15` attributes to bgfx's instance data buffer and
  implements `glVertexAttribDivisor`.
  Diagnostics that record individual draws (`cne_capture`, `CNE_METAL_SHOW_WORLDCORNER`)
  still do not see per-batch details.

The remaining work is **multi-day architectural / optional** and is tracked in the
[roadmap in README.md](../README.MD#11-future-work-roadmap-not-scheduled): Metal-native
`Context3D`, dropping bgfx, deleting the GL semantics adapter, and 3D/Stage3D support.
