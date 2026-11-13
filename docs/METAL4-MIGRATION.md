# Metal 4 migration — status and plan

## Current status

- Dependencies updated to the latest upstream snapshots (2026-11-13):
  - bgfx `eebf5686a8c5f1c77e1af2dad17b5746ba17da8f` (API **v168**)
  - bx `57548da89a07771c5b0f8a6711149be70e08a329`
  - bimg `22177183239e18d7af603e6915156371cc38d18d`
- Both `osx-arm64` and `osx-x64` static libraries were rebuilt and the Metal backend
  compiles against them.
- The backend still uses bgfx's Metal renderer (Metal 2-era API).  It does **not** use
  Metal 4 APIs.

## Why Metal 4 cannot be enabled by a dependency update

Metal 4 (`MTL4CommandQueue`, `MTL4CommandAllocator`, `MTL4CommandBuffer`, `MTL4CommandEncoder`,
`MTL4Compiler`, argument tables, …) is a new command/encoder model.  bgfx's current Metal
renderer (`src/renderer_mtl.cpp` at `eebf568`) contains **no MTL4 usage**; it submits through
`MTLCommandQueue`/`MTLCommandBuffer`/`MTLRenderCommandEncoder`, i.e. the previous API.

The macOS 26 SDK on this machine does provide the `MTL4*.h` headers, so the SDK side is ready.
Using them requires replacing the bgfx Metal renderer (or adding a parallel native renderer),
not upgrading a version number.

## Options

### A. Keep bgfx (recommended short term)
- Stable, already validated by the current release.
- No Metal 4 features; 2D rendering does not need them.

### B. Native Metal 4 renderer behind a feature flag
Add a parallel renderer (e.g. `CNE_METAL_API4=1`) and keep bgfx as the fallback:

1. **Phase 0 — probe + design**
   - Detect `MTL4CommandQueue`/`MTL4CommandAllocator` availability and the Metal 4 compiler.
   - Define the boundary between the existing Lime/GL-shaped API and a native MTL4 backend.
2. **Phase 1 — present path**
   - `CAMetalLayer` + `MTL4CommandQueue`/`MTL4CommandAllocator`, clear and present a frame.
3. **Phase 2 — 2D pipeline**
   - Textures/samplers/vertex buffers, pipeline states, draw submission for the GL-shaped
     draw entry points.
4. **Phase 3 — render targets/filters/AOT**
   - FBO equivalents, copy/blit paths, and `MTL4Compiler`-based shader compilation replacing
     the bgfx shader-bin path.
5. **Phase 4 — validation/performance**
   - Pixel regression, mod shader translation, filters, resize, release packaging.

Estimated effort: weeks of focused work, with high regression risk.  It should be developed
on a separate branch and **must not block the bgfx-based release**.

## Decision

The dependency update is complete; the Metal 4 renderer is a separate project.  Do not claim
"Metal 4 support" until a native MTL4 present path and 2D pipeline are implemented and tested.
