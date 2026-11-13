# Native Metal 4 renderer (bgfx removal)

## Progress: ~90%

Milestones: probe ~5%, present+clear ~15%, first textured 2D draw ~25%, multiple draws
~30%, ortho + two textures + blend ~35%, renderer core ~40%, drawable present via core
~45%, batched 2D draw with alpha ~50%, pipeline cache ~55%, render-to-texture + filter pass
~60%, scissor/viewport ~65%, AOT metallib pipeline ~70%, GL-shaped backend ~80%,
parity + bgfx removal 100%.

## Status

- Branch: `metal4` (main stays on the bgfx renderer).
- `metal-cpp` is vendored at Apple's `27c4382b7151d55a51692cdcb27aaa98752240de`.
- A standalone Metal 4 probe builds and runs on this machine.  The present probe also creates a
  `CAMetalLayer`, clears to a colour and presents 30 frames through an `MTL4CommandQueue`.
- The sprite probe renders two textured quads with two draw calls into an offscreen target and
  verifies readback (left/right red, blue background).
- The scene probe adds an orthographic projection uniform, two textures bound through separate
  argument tables and alpha-blend pipeline state; readback confirms positions and colours.
- `metal/tools/metal4_renderer.{hpp,mm}` is the first reusable core: device/queue/allocator,
  frame pass, textures/buffers/samplers, pipeline + argument table binding, indexed/non-indexed
  draws, event-synchronised frame end and readback.  The renderer test reproduces the scene
  output (`bg=(0,0,51) red=(255,0,0) blue=(0,0,255)`).
- `metal4_window` drives the same renderer against a real `CAMetalLayer` drawable and presents
  120 frames (`presented=120`).
- The batch probe draws 100 quads (600 vertices) in one call with per-vertex colour and alpha
  blending; readback confirms the blended colour (`sprite=(128,0,13)` over `bg=(0,0,26)`).
- `Renderer::getPipeline()` caches pipeline states by vertex/fragment source, blend mode and
  colour format; the cache test reuses one pipeline and creates a second only for a different
  blend mode (`count=2`).
- The filter probe renders a quad into an offscreen render target, then samples that target in
  a second pass into the final target; readback: `center=(255,0,0)`, `corner=(0,0,255)`.
- Scissor and viewport are exposed on the renderer; the scissor probe clips a full-screen quad to
  the left half (`left=(255,0,0) right=(0,0,255)`).
- AOT metallibs load through `MTL::Library` and expose the translated entry point `xlatMtlMain`.
- The app itself can now present through MTL4: `Metal4Clear.mm` creates an MTL4 queue on the
  existing `CAMetalLayer` and clears/presents when `CNE_METAL4_CLEAR=1`.  Game draws are still
  bgfx-encoded, so this only verifies the app/window presentation hand-off.  The hook now also
  draws a full-screen MTL4 quad with an MSL pipeline, proving app-side MTL4 draw submission.
  The quad now samples a 2x2 texture through an MTL4 argument table (texture + sampler).
- The engine's native texture-upload path (`cne_metal_tex_image_2d`) forwards level-0 RGBA
  uploads to the MTL4 hook, which keeps the latest engine texture and samples it in the app
  window.  This proves real engine texture data reaches MTL4 resources.
- `Renderer::getPipelineFromMetallib()` builds a real pipeline from the generated VS/FS
  `.metallib` pair; the metallib-pipeline probe renders red (`center=(255,0,0)`), so the AOT
  shader path is usable from MTL4 without recompiling MSL at runtime.
- The quad probe renders a textured full-screen quad offscreen through a Metal 4 render encoder
  (Metal 3 PSO + `MTL4::ArgumentTable`, `drawPrimitives`), then reads the target back:

```text
METAL4 PROBE: device=Apple M3 metal4=YES
METAL4 PROBE: MTL4 command queue/allocator/command buffer OK
```

Build/run (add `--present` for the CAMetalLayer clear/present test):

```sh
CodenameEngine-1.0.1/metal/scripts/build-metal4-probe.sh --present
```

## Why bgfx cannot be deleted yet

The current renderer is a bgfx-facing implementation of the Lime/OpenGL-shaped API.  Removing
bgfx means implementing that API directly on Metal 4: command queue/allocators, render passes,
pipeline states, argument tables, buffers/textures/samplers, uniforms, render targets, view
ordering and presentation.  Deleting the dependency before that path exists leaves the app
with no renderer.

## Metal 4 requirements

- **macOS 26+** at runtime (`MTLGPUFamilyMetal4`).  The current release still supports macOS 11.
- The existing deployment target remains 11.0 while the bgfx path is the shipping renderer;
  a Metal 4-only build would have to raise it to 26.0.

## Migration phases

1. **Probe / design (done)** — metal-cpp vendored, MTL4 device/queue/allocator/command
   buffer verified.
2. **Present + clear (done)** — `CAMetalLayer` drawable, MTL4 command queue, clear pass, present.
   **First textured 2D draw (done)** — vertex buffer, sampler, texture, pipeline and argument
   table; readback is exactly 32768 orange / 32768 blue checker pixels.
3. **2D pipeline** — textures/samplers, vertex/index buffers, pipeline states, uniforms,
   draw submission for the GL-shaped entry points.
4. **Targets/filters** — render targets, attachment caches, filter passes, UV flip rules.
5. **Shaders** — load the existing MSL/`metallib` output through `MTL4Compiler`/libraries
   (and keep the runtime mod-shader translation path).
6. **Parity + switch** — validate every mode, then remove the bgfx renderer and the
   `bgfx-master` build dependency.

## Reference material

- Apple Game Porting Toolkit skills (Metal 4 API, resources, synchronization, pipeline
  creation, drawable presentation) — used as the API reference for this migration.
- `metal-cpp` headers: `metal/third_party/metal-cpp/Metal/MTL4*.hpp`.

## Ground rules

- Do not delete the bgfx build until the Metal 4 path renders the game and passes regression.
- Keep the Metal 4 path behind a feature flag/branch so releases remain buildable.