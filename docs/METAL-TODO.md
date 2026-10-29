# Metal backend TODO

**English** | [中文](zh-CN/METAL-TODO.md)

**No open actionable items right now.** Everything that can be completed and verified with a
single app build is done: mask (runtime pixel proof), `readPixels` (offscreen immediate +
one-frame backbuffer cache), deterministic pixel-regression goldens, AOT coverage and the
runtime-translation policy decision.

Baseline: **114 / 116** stages ship a prebuilt `.metallib` (verified by
`metal/scripts/check-docs.py` and `gen_shader_aot.py --strict`).

The remaining work is **multi-day architectural / optional** and is tracked in the
[roadmap in README.md](../README.MD#11-future-work-roadmap-not-scheduled): Metal-native
`Context3D`, dropping bgfx, deleting the GL semantics adapter, and 3D/Stage3D support.
