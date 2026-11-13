# Performance optimizations

The backend contains several groups of draw-path optimizations (A–G) plus draw merging (D38).

## A. Per-draw fast paths

- Cached view rect / scissor / clear state.
- FBO fast-path cache with generation counters.
- Cached bgfx state, dirty-uniform uploads.
- Cached sampler flags; bound-texture lookup arrays.
- Parsed diagnostic skip lists instead of re-parsing environment strings.

## B. Texture path

- Data textures are created without `BGFX_TEXTURE_RT`; same-size re-uploads use `updateTexture2D`.
- Textures are promoted to render targets when attached to a framebuffer.
- CPU mirrors are only kept where needed; large static mirrors are released.

## C. Render targets / presentation

- FBO attachment caches invalidated on attachment changes.
- View-order array bounded to bgfx's 256 views.
- Shared depth pool is available as an opt-in experiment (`CNE_METAL_SHARED_DEPTH=1`) but disabled by default because filter chains can create several same-size FBOs.
- The full backbuffer is cleared every frame for correctness.

## D. Submission path

- Cached max index per index buffer (D37).
- **Draw merging (D38):** consecutive compatible indexed draws are merged through a merged-index cache, including disjoint ranges in the same or different index buffers (default on; `CNE_METAL_MERGE_DRAWS=0` disables).
- Fast bound-texture lookup on the draw path.

## E. Shaders / AOT

- In-process memoization of runtime-translated mod shaders.
- Single-pass shader hash (FNV-1a + DJB2 in one loop).
- 59/59 AOT stages.

## F. Window / presentation

- Default 1600x900 window clamped to 94% of the visible screen, 16:9 preserved.
- `window.restorable = NO`; per-frame drawable-size polling; Stage Manager/visibility surface restore; high-DPI drawable handling.

## G. Resource lifecycle

- GL buffer caches and framebuffer attachment caches are released on delete.
- All handler tables are cleared on context shutdown.

## Instanced bitmap batching (opt-in)

`openfl-metal` can batch consecutive compatible `Bitmap` draws into one `drawElementsInstanced` call. It is **opt-in** in this release because it does not yet apply the offscreen late matrix flip, which can rotate/fragment filtered stages.

```sh
CNE_METAL_INSTANCING=1 /path/to/CodenameEngine.app/Contents/MacOS/CodenameEngine
```

or `FlxG.metalInstancing = true`.

## Useful switches

```sh
CNE_METAL_MERGE_DRAWS=0      # disable draw merging
CNE_METAL_INSTANCING=0       # disable instancing (default)
CNE_METAL_SHARED_DEPTH=1     # enable shared depth pool (experimental)
CNE_METAL_RT_TEXTURES=1      # force all textures to be render targets
CNE_METAL_LEGACY=1           # disable A/C draw-path caches
CNE_METAL_TRACE=1            # trace submit/bind/flush/frame
```
