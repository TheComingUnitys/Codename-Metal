# Metal backend TODO (remaining work)

**English** | [中文](zh-CN/METAL-TODO.md)

> Everything that could be completed with an app build has been done and verified:
> mask (runtime pixel proof), `readPixels` (offscreen + one-frame screen cache),
> deterministic pixel-regression goldens, and the runtime-translation policy decision.
> What remains below needs multi-day architecture work or external input.
>
> Diagnostics: [DIAGNOSTICS.md](DIAGNOSTICS.md) - verification: `./verify.sh` -
> pixel regression: `metal/autotests/README.md`.

## 0. Current baseline (auto-checked, do not edit by hand)

| Item | Value |
| --- | --- |
| Prebuilt `.metallib` | **114 / 116** |
| Build-time GLSL to MSL failures | **2 stages** |

### 2.1 The 2 stages that still fail at build time (external shader source issues)

| key | stage | Reason |
| --- | --- | --- |
| `EFD80EB711B13CBD` | fragment | calls the undeclared function `noise1()` |
| `B3064EBB937D8A8F` | fragment | `gl_FragColor = vec2` (invalid GLSL) |

Both are external shader-source bugs; this repository does not modify those sources.

---

## 1. Multi-day architecture work (P0)

| Sub-item | State | Notes |
| --- | --- | --- |
| Metal-native `Context3D` (drop the GL-shaped `lime_gl_*` API and the C++ GL state machine) | not started | plan in `docs/zh-CN/metal/audit/openfl-native.md`; 2-3 days |
| Remove bgfx and submit to Metal directly | not started | same-frame A/B direct-Metal replay prototype exists (`metal/tools/native_replay/`, pixel-identical) |
| Delete the GL semantics adapter (Y flip, FBO attachment cache, matrix mirroring) | blocked on the two items above | `MetalBindings.cpp`, `MetalNative.inc` |

## 2. 3D / Stage3D (P1, as needed)

3D texture upload, `glGenerateMipmap`, MRT/drawBuffers, blitFramebuffer, clearBuffer,
transform feedback, uniform blocks, queries and instancing are not implemented.
Each requires a 3D test scene for validation.

## 3. External blockers

| Item | Blocker | Unblock |
| --- | --- | --- |
| 2 shader stages | external shader sources contain invalid GLSL (see 2.1) | fix upstream |
| GitHub Actions workflow | current `gh` token lacks the `workflow` scope | `gh auth refresh -s workflow`, then copy `docs/ci/metal-verify.yml` to `.github/workflows/` |

## 4. Recently completed (context, needs a build)

- `DisplayObject.mask` verified end-to-end: `CNE_METAL_SELFTEST=1` renders a masked
  OpenFL sprite, checks `Window.readPixels` and `BitmapData.draw` pixels (inside white,
  outside masked) and exits 0 when both pass.
- `lime_gl_read_pixels` implemented: offscreen FBOs use a READ_BACK texture + `bgfx::read`
  (immediate); the backbuffer uses a one-frame cache (first call arms, next call returns).
- Deterministic pixel-regression goldens: `--scenario selftest` (0 tolerance) and
  `--scenario menu` pass repeated runs (size differences are normalised by rescaling).
- Runtime translation fallback decision: keep ON. A build+mod run with
  `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` had 116 cache entries and no misses, but third-party
  shaders installed *after* a build are not in the cache, so defaulting it off would break
  those shaders. `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` remains available for strict runs.

## 5. Quick entry points

```sh
./verify.sh
./metal/scripts/diag.sh --list
CNE_METAL_SELFTEST=1 CNE_METAL_SELFTEST_EXIT=1 <app>/Contents/MacOS/CodenameEngine
./metal/scripts/pixel-regression.sh --app <app> --scenario selftest
```
