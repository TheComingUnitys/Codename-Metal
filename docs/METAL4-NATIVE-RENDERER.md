# Native Metal 4 renderer (bgfx removal)

## Progress: ~40%

Milestones: probe ~5%, present+clear ~15%, first textured 2D draw ~25%, multiple draws
~30%, ortho + two textures + blend ~35%, renderer core ~40%, full 2D pipeline ~50%,
targets/filters ~70%, shaders/AOT ~85%, parity + bgfx removal 100%.

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