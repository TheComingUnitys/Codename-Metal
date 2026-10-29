# Metal backend TODO (only build-blocked and externally blocked items)

**English** | [中文](zh-CN/METAL-TODO.md)

> Every task that can be completed **without building the app** is done (translator,
> AOT, diagnostics/testing, release pipeline, Lime patch export, state-semantics
> decisions, ...).
> Each item below states why it needs a build/run cycle or an external dependency.
>
> Diagnostics: [docs/DIAGNOSTICS.md](DIAGNOSTICS.md) - Lime patches:
> [docs/LIME-PATCHES.md](LIME-PATCHES.md) - verification: `./verify.sh`.
> Chinese version: [docs/zh-CN/METAL-TODO.md](zh-CN/METAL-TODO.md).

## 0. Current baseline (auto-checked, do not edit by hand)

`metal/scripts/check-docs.py` verifies these numbers; `.metallib` coverage is verified by
`gen_shader_aot.py --strict` against `metal/audit/aot-expected.json`.

| Item | Value |
| --- | --- |
| Cached shader stages | 116 (16 vertex / 100 fragment) |
| Prebuilt `.metallib` | **114 / 116** |
| Build-time GLSL to MSL failures | **2 stages** |

### 2.1 The 2 stages that still fail at build time (external shader source issues)

| key | stage | Reason |
| --- | --- | --- |
| `EFD80EB711B13CBD` | fragment | calls the undeclared function `noise1()` (the source only defines `noise1_2`) |
| `B3064EBB937D8A8F` | fragment | `gl_FragColor = vec2` (vec4 = vec2, invalid GLSL) |

Both problems live in external shader sources; this repository does not modify them.
Per-stage details: `metal/audit/aot-coverage.md` and `metal/audit/aot-expected.json`.

The engine side has no known translation defects left; any new failure is caught by the
`--strict` audit.

---

## 1. Needs an app build + runtime pass (paused while app builds are disallowed)

### 1.1 `DisplayObject.mask` runtime verification
Engine side is complete: `bgfx::setStencil`, `colorMask -> BGFX_STATE_WRITE_*` and a
renderbuffer D24S8 stencil attachment.
**Needed:** one app build, then a real `.mask` scene plus a pixel-regression capture
(`CNE_METAL_SHOT_RAW`) proving the mask shape is invisible and the content is correct.

### 1.2 `read_pixels` (screenshots / readback)
`lime_gl_read_pixels` is still a no-op (it now emits a one-shot `ReportUnimplemented`
warning instead of silently returning zeros). bgfx readback is asynchronous, so synchronous
GL semantics need frame-boundary handling (`BGFX_TEXTURE_READ_BACK` + `BGFX_FRAME_FLUSH`,
or a CAMetalLayer drawable snapshot). **Any implementation must be validated in a running
app** (both screenshots and `BitmapData.draw(renderer)`), hence it belongs here.

### 1.3 Make the runtime-translation fallback default off
`ShaderAot.isRuntimeFallbackEnabled()` currently defaults to on. Flipping it requires a run
with a real third-party shader set (including "shader added only after the build"), proving
there is no unknown translation dependency; otherwise newly added shaders fail outright.

### 1.4 Architecture items 1.1/1.2/1.4 (P0)
- 1.1 Metal-native `Context3D` (drop `lime_gl_*` and the GL state simulation);
- 1.2 remove bgfx and submit to Metal directly;
- 1.4 delete the GL semantics adapter (Y flip, FBO attachment cache, matrix mirror fix-ups, ...).
All three touch the main render path and require build-and-run regression coverage.

### 1.5 3D / Stage3D capabilities
3D texture upload, `glGenerateMipmap`, MRT/drawBuffers, blitFramebuffer, clearBuffer,
transform feedback, uniform blocks, queries, instancing: not implemented. Each must be
validated with a 3D test scene once implemented.

### 1.6 Pixel-regression goldens
The tooling is ready (`frame_diff.py`, `pixel-regression.sh`, self tests wired into
`verify.sh`) but there are **no golden frames** yet. After one app build run:
`pixel-regression.sh --app <app> --scenario menu --update`.

---

## 2. External blockers (not build-related)

| Item | Blocker | Unblock |
| --- | --- | --- |
| 2.1 The last 2 shader failures | external shader sources contain invalid GLSL (see section 0) | fixed upstream; this repo will not edit external sources just to raise the number |
| GitHub Actions workflow | the current `gh` token lacks the `workflow` scope, so `.github/workflows/` cannot be pushed | `gh auth refresh -s workflow`, then `cp docs/ci/metal-verify.yml .github/workflows/` |

---

## 3. Suggested order once unblocked

1. Build the app: `./metal/scripts/make-universal.sh --check`, then `--dmg`;
2. Generate pixel-regression goldens (1.6) and verify 1.1 (mask) at the same time;
3. Implement and measure 1.2 (readPixels);
4. Flip the runtime-translation fallback (1.3) with a real third-party shader regression;
5. 1.1 (Context3D) -> 1.2 (drop bgfx) -> 1.4 (delete the adapter);
6. 1.5 (3D) as needed.

## 4. Quick entry points

```sh
./verify.sh                                   # translator tests + strict AOT audit + docs + self tests
./verify.sh --with-native                     # plus the C++/bgfx compile check
./metal/scripts/diag.sh --list                # diagnostics categories
./metal/scripts/pixel-regression.sh --selftest
./metal/scripts/export-lime-patches.sh --lime ... --pristine ... --out ... --verify
```
