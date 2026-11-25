# Metal 4 native draw — black-screen diagnosis and fix (2026-11-16)

Status (2026-11-17): the native path now renders the game scene stably; the
black-screen / flashing-picture bug is fixed. The default bgfx path is untouched.
Gameplay / menu / pause / FBO-filter / resize regression is still pending, so the
native path is not yet declared complete.

## Root cause of the black engine draws

The engine's MSL attribute slots are assigned by the GLSL→MSL translator in
**declaration order**, not by the OpenFL attribute name. For the 5- and 8-attribute
OpenFL display shaders the AOT cache says:

```text
openfl_Alpha          -> a_position  [[attribute(0)]]   float
openfl_ColorMultiplier-> a_normal    [[attribute(1)]]   float4
openfl_ColorOffset    -> a_tangent   [[attribute(2)]]   float4
openfl_Position       -> a_bitangent [[attribute(3)]]   float4
openfl_TextureCoord   -> a_color0    [[attribute(4)]]   float2
alpha                 -> a_color1    [[attribute(5)]]
colorMultiplier       -> a_color2    [[attribute(6)]]
colorOffset           -> a_color3    [[attribute(7)]]
```

`MetalNative.inc` mapped names with `OpenflToMslName()` and fed `openfl_Position`
into `a_position` (slot 0). That slot is actually `openfl_Alpha`; the real position
input (`a_bitangent`, slot 3) received a dummy `float4` from the first vertex. Every
engine triangle therefore collapsed to a single point even though the pipeline, FBO,
indices, uniforms and present were all fine. This also invalidated the previous
`CNE_METAL4_DEBUG_ENGINE` experiment, which drew pixel-space positions straight into
clip space.

Fix: use the translator's authoritative `glLocation -> MSL slot` table
(`attribSlots`, pushed by `lime_metal_configure_program`) and collect every
alpha/colour constant attribute (both `openfl_*` and Flixel's lowercase names)
before walking the VAO.

## Metal 4 issues fixed at the same time

- `MTL4ArgumentTableDescriptor.maxBufferBindCount` is exclusive. The constant-step
  block is bound at `bufferCount + 1`, so the table needs `bufferCount + 2` slots;
  the old bound made every constant bind an API error and left alpha at 0.
- Metal 4 command buffers **do not retain resources**. Per-draw argument tables,
  index buffers, uniform/constant buffers and replaced textures were locals and
  could be freed before the GPU ran. They are now kept alive until the next frame
  begin (after a completion event) and tracked by residency sets.
- `frame_begin` is idempotent per frame; it used to clear the transient anchors on
  every target switch, releasing buffers from draws already encoded in the same
  command buffer.
- Uniforms now carry their stage from the Haxe bridge, so VS and FS MSL structs at
  `buffer(0)` are filled at their real offsets instead of being split by guessing.
- Textures are always created with `RenderTarget` usage (Metal usages are
  immutable; an FBO texture may have been sampled before it is attached).
- Viewport/scissor Y is flipped from GL's bottom-left origin only for sub-rects.
- The magenta `CNE_METAL4_DEBUG_ENGINE` shader now reads slot 3 and applies a
  viewport ortho, so it is a meaningful diagnostic.

## Verification

```sh
CodenameEngine-1.0.1/metal/scripts/run_metal4_selftest.sh
```

Headless, offscreen, Metal API validation on. It checks a 5-attribute-style layout
(position on MSL slot 3, UV on 4, alpha constant on 5), an indexed draw, separate
VS/FS uniform blocks, the constant block at `bufferCount + 1`, Float2 and Float3
position descriptors feeding a `float4` MSL input, two frames, and CPU readback.

Result:

```text
[selftest] frame 0 center pixel = 128 0 128 128
[selftest] frame 1 center pixel = 128 0 128 128
[selftest] PASS
```

## Play-test

```sh
CNE_METAL4_NATIVE=1 ~/Downloads/CodenameEngine-arm64-m4native.app/Contents/MacOS/CodenameEngine \
  > /tmp/cne-native.log 2>&1
```

The first draws should log `[cne-metal4] attrmap ...` with `gl0->msl3` (position)
and `vd ... loc3->buf1 ... cloc0->buf2 ...`. Default bgfx path:

```sh
~/Downloads/CodenameEngine-arm64-m4native.app/Contents/MacOS/CodenameEngine
```

## Known gaps (not black-screen blockers)

- Per-vertex alpha/colour attributes (and `genericAttribs`) are currently forced to
  neutral values, so sprite fades/colour tints are ignored.
- One global linear/clamp sampler; OpenFL's per-texture filter/wrap is not applied.
- `cne_metal_draw_elements_instanced` still goes through the bgfx path; instance
  attributes (`i_dataN`, slots 16+) are not wired into the native descriptor.
- Sampler slots are mapped by GL texture unit; multi-sampler custom shaders may
  need an explicit slot binding.
- Blend equation (only add) and depth/stencil are not translated.


## 2026-11-17: why the picture flashed

After the attribute/texture fixes, the offscreen chain was correct: FBO 54 and
FBO 66 read back ~95% non-black, while the drawable was black on most frames and
only occasionally contained the scene. The screen composite (`prog=4`, sampling
FBO 66) sampled the FBO in a later encoder without any ordering relative to the
FBO render pass. Metal 4 encoders do not implicitly order cross-encoder reads.

Fix: at the start of every render encoder, encode

```objc
[gEncoder barrierAfterQueueStages:(MTLStageVertex | MTLStageFragment)
                     beforeStages:(MTLStageVertex | MTLStageFragment)
                visibilityOptions:MTL4VisibilityOptionDevice];
```

That makes the screen encoder's fragment sampling wait for the FBO encoder's
writes and flushes caches. The window is now stable; the self-test and a real
`CNE_METAL4_VERBOSE=1` run show a clean target/draw sequence. The readback path
(`CNE_METAL4_READBACK=1`) is diagnostic only and perturbs timing; its compute
copy can read black, so do not use it to judge normal rendering.

## 2026-11-23: Freeplay portrait / camera-layer composition

**Symptom.** Freeplay's right-hand TV shows no portrait in the native path;
gameplay can lose the notes/UI because a later camera layer paints an opaque
black rectangle over them.

**Root cause.** `lime_gl_bind_framebuffer` cleared every render target it bound
with the *current* `glClearColor`.  At bind time that is the previous pass's
colour (frequently opaque black), not the colour the target is about to be
cleared with.  The Freeplay main camera draws `bg_freeplay.png`, which has a
deliberately transparent TV window (`x 1160..1716`, `y 280..814`); clearing the
camera canvas to opaque black filled that window and hid the `tvScreen`
portrait underneath.

The bgfx reference path does not do this: `ConfigureView()` clears every
offscreen view to **transparent black** (`CNE_METAL_RT_TRANSPARENT_CLEAR`,
default on), so the window stays transparent and the lower camera layer shows
through.

**Fix (native only; `cne_metal4_app_active()` still gates everything).**

- FBO binds now record a *pending* target + transparent clear and open the
  Metal render pass lazily on the first draw (`Metal4App.mm EnsureEncoder`).
  A freshly created attachment is cleared transparent on first use.
- Explicit `glClear` is routed to `cne_metal4_app_clear`, which updates the
  pending clear (or closes and reopens the pass if a draw already happened).
- The screen clear remains deferred by screen-last and is updated by the
  explicit stage clear (`cne_metal_clear`).

**Verification.** Run with `CNE_METAL4_VERBOSE=1`.  In Freeplay the main-camera
target (e.g. `166`) must now log `clear=1 rgba=0.00,0.00,0.00,0.00` for its
content bind instead of `0.00,0.00,0.00,1.00`, and the TV window must show the
portrait.  Default bgfx is unchanged because the new code is only reached when
the Metal 4 app renderer is active.

## 2026-11-23: hxvlc cutscene video stays black

`hxvlc`'s `Video` creates its texture with
`context3D.createRectangleTexture(w, h, BGRA, /* optimizeForRenderToTexture */ true)`.
That gives the texture an FBO attachment even though the content is uploaded
from the CPU every frame.  The native sampler classified any attachment as a
GPU-only render target, so it stopped uploading the CPU mirror after the first
(empty) allocation - the video texture stayed black.

Fix: add `TextureData::gpuDrawn`, set when a framebuffer owning the texture is
actually bound (`lime_gl_bind_framebuffer`) and cleared on every `glTexImage2D`.
A texture is treated as GPU-backed only when it is attached **and** has been
bound (or has no CPU data at all).  hxvlc's unused FBO therefore keeps
`gpuDrawn == false`, and its per-frame uploads reach the Metal texture.  The
same flag keeps the video out of the main-RT V-flip detection.

No Haxe change; the default bgfx path is unaffected.

## 2026-11-24: "wrong assets" while a macOS overlay (volume HUD) has focus

**Symptom.** Pressing F11/F12 (or otherwise raising a translucent system overlay)
corrupted the frame: sprites belonging to another state appeared, glyphs/quads
were sliced and mispositioned.  The picture recovered as soon as the overlay
went away.

**Root cause (log-proven).** The per-frame order is

```text
frame_end f=N  ->  texture / vertex uploads  ->  target f=N screen=1
```

and only that last step enters `frame_begin`'s `WaitForGpu()`.  The engine
therefore rewrites the `MTLStorageModeShared` textures and vertex/index buffers
for frame N+1 while the GPU may still be sampling them for frame N.  Metal 4
performs no cross-frame dependency tracking, so the GPU read half-overwritten
data.  `CNE_METAL4_READBACK=1` masked the bug purely because it waits for the
GPU at every `frame_end` (full CPU/GPU lockstep).

An early focus theory was wrong: `[cne-ev] window` showed focus and size
unchanged in the corrupted runs.  One run did catch `f=2077 focused=0 ->
f=2079 focused=1`, i.e. the overlay does steal focus briefly, but that is a
timing change, not the cause.  `drawable nil`, texture `replace` and id reuse
were all ruled out (ids are monotonic and never recycled).

**Fix.** `Metal4App.mm` gained `WaitBeforeCpuWrite()`: before the first CPU
write of a frame - a texture upload (`EnsureTexture` with non-null pixels) or
a vertex/index buffer rewrite (the `cne_metal4_app_buffer` memcpy branch) - it
waits on the previous frame's completion event.  At most one wait per frame,
and only on frames that actually write.

**Status.** Confirmed by the user: F11/F12 no longer corrupts the picture.
Do not remove this wait; it is also the safety net for future upload races.

## 2026-11-24: title screen's white flash renders as a 4-second black veil

**Symptom.** After the intro subtitles ("In association with newgrounds") the
title state is supposed to open with one white flash - upstream
`source/funkin/menus/TitleState.hx` really is
`FlxG.camera.flash(FlxColor.WHITE, 4)` in CNE `v1.0.1`, so the 4-second
duration is original.  The native path instead showed a black veil fading out
over those 4 seconds.

**Two stacked bugs.**  The fill/graphics shader computes

```text
color = clamp(offset + (texel / texel.a) * colorMultiplier);
out   = color * color.a * alpha;
```

`CNE_METAL4_DRAWLOG=1` caught the flash draw as `prog=11 tex=12 buffers=1
attribs=2 consts=3`, with

```text
tex: 12                        <- 1x1 rgba=000000ff (BLACK)
const slot=0 comps=1 val=0.988  <- alpha, decaying over 4s
const slot=1 comps=4 val=1,1,1,1
const slot=2 comps=4 val=0,0,0,0
```

1. `MetalNative.inc`'s `isColorConstantSlot()` discarded the real
   `colorMultiplier`/`colorOffset` the engine had set with `glVertexAttrib4f`
   and substituted white/0.  That was originally done to suppress a
   rainbow-tinted white loading spinner, but that artifact was really the
   stride-aliasing bug, fixed separately.
2. The shape draw's 1x1 texture is the same *black* 1x1 the camera background
   fill uses, so `black texel * white multiplier` stays black.

**Fix.**

- Generic colour attributes now use their real values (`addConst` with the
  actual `v0..v3`); diagnostics: `[cne-metal4] coloruse ...` behind
  `CNE_METAL4_EVENTS=1`.
- For shape draws (program without a `hasTransform` uniform) that sample a 1x1
  texture, the sampler is pointed at a built-in white 1x1 (`id -7777`, uploaded
  as `ffffffff`).  Then the camera's background fill gets its black multiplier
  and stays black, while the flash gets a white texel plus the white multiplier.
  On by default; `CNE_METAL4_SHAPE_WHITE=0` restores the old binding for A/B.

**Status.** Built 2026-11-24 21:25, waiting for in-game confirmation.  This is
the highest-risk change in the tree: every solid fill (menu fade transitions,
`pressEnter()`'s `flash(WHITE, 1)`, in-game lighting/colour fills) now uses a
real multiplier, so those need regression too.

## 2026-11-25: that fix bound *no* texture at all

After the 21:25 build the title showed **neither** the white flash nor the old
black veil - the fill draw had become invisible.  Cause: the built-in white 1x1
is created under a fake id (`-7777`) and simply assigned to `tex`, but the code
then fell through into the ordinary sampler path.  That path fills
`textureIds[]`, decides dirty uploads and tracks versions, and it only runs for
ids present in the GL-side `textures` registry - `-7777` is not in it, so the
whole `if` was skipped, `textureCount` stayed at 0 and the draw sampled an
*unbound* slot.  `texel / texel.a` then evaluates 0/0, the fragment goes
transparent and nothing is drawn.

Fix: bind the white 1x1 straight into `textureIds` for that unit and `continue`,
skipping the registry path entirely.  The flash is now
`white texel * real white multiplier * real alpha`, i.e. an opaque white
overlay fading out over 4 seconds - which is what upstream
`FlxG.camera.flash(FlxColor.WHITE, 4)` means.  Note there is no separate
"black curtain" in the upstream title: `blackScreen` is an opaque sprite that
covers the intro subtitles and is removed in the same frame the flash starts,
so a *fading* black veil was always the bug, not the intent.

## 2026-11-25: gameplay stutter - per-draw GPU object churn

**Symptom.** Gameplay ran but stuttered; the native path felt far less smooth than
bgfx.

**Root cause.** Every single draw in `cne_metal4_app_draw` created GPU objects:

```text
2x [MTL4ArgumentTableDescriptor new] + newArgumentTableWithDescriptor:
1x newBufferWithBytes  (vertex-stage uniform block)
1x newBufferWithBytes  (fragment-stage uniform block)
1x newBufferWithBytes  (constant-step alpha/colour block)
1x newBufferWithBytes  (index data)
```

A frame with several hundred draws therefore did thousands of allocations,
thousands of residency-set inserts (`MarkFrameResident` does a membership test
against the set on every call, so cost grew with the number of live frame
allocations), a `[gResidentFrame removeAllAllocations]` + commit every frame, and
the matching releases a frame later - all of it pure allocator and bookkeeping
churn, none of it drawing.

**Fix (all in `Metal4App.mm`).**

- **Frame scratch arena.** One Shared `MTLBuffer` (4 MB, doubling) hands out
  256-byte aligned slices; the vertex/fs uniform blocks, the constant block and
  the index data are now `memcpy`'d into it. It is reset at `frame_begin`, i.e.
  *after* `WaitForGpu()`, so a slice is never rewritten under an in-flight draw.
  If it has to grow mid-frame the old buffer is kept alive until the next
  `frame_begin` retires it, because earlier draws still address it.
- **Argument table pool.** Tables are allocated once per `(frame, draw slot)` and
  reused on every later frame; a slot is only handed out again after the GPU has
  finished, so no table changes under an in-flight draw. One fixed descriptor
  (`maxBufferBindCount = 18`, 16 textures, 1 sampler) fits every draw, so the pool
  never has to be re-shaped. Bindings a draw does not set are harmless - the
  pipeline decides what is actually read, and the GL layer always binds every
  stream / constant / texture its program declares.
- **No per-draw residency registration.** The arena is persist-resident, so the
  frame residency set stays empty (`MarkFrameResident` is kept but unused).
  The vertex streams still point at the persistent `gBuffers` entries.
- **Pipeline cache key.** `key.reserve (2048)` - the key is rebuilt for every draw
  and used to reallocate a dozen times per draw.

**Instrumentation.** `CNE_METAL4_PERF=1` prints one line every 120 frames:

```text
[cne-perf] frames=120 avg=6.31ms max=19.44ms draws/frame=412 arenaPeak=412.0KB tables=412
```

`tables` stabilises after the first frame (it equals the busiest draw count seen
so far, and stops growing) and `arenaPeak` should stay around 1 KB per draw - that
is the evidence the per-draw allocations are gone. `max` is the stutter metric:
it now tracks real hitches (loading, shader work) rather than allocator churn.

Nothing here changes rendering: same draw order, same bindings, same data, and
the whole path is still gated by `cne_metal4_app_active()`.

## 2026-11-25: black fades rendered white (the white-1x1 substitution needed a guard)

**Symptom.** After the white-flash fix, transitions that should fade to **black**
(freeplay -> playstate) came out **white**.

**Why.** The generated fill shader only multiplies by the colour when the uniform
`openfl_HasColorTransform` is set:

```metal
color = s_texture.sample(...);
if (color.a == 0.0)        out = 0;
else if (hasColorTransform) out = clamp(offset + color/color.a * mult) * a * alpha;
else                        out = color * alpha;      // the texel IS the colour
```

A capture with `CNE_METAL4_EVENTS=1` shows both cases in the same run:

```text
[cne-ev] fill prog=11 hct=1 tex=12 texel=ff000000 subst=1 s1(4c)=1,1,1,1   <- title flash
[cne-ev] fill prog=11 hct=0 tex=12 texel=ff000000 subst=1 s1(4c)=0,0,0,0   <- the rest
```

(`ff000000` is the little-endian word for a black 1x1.)  The white 1x1 stand-in is
only valid in the first case, where the colour rides on the multiplier and the
1x1 is a dummy; with `hasColorTransform` off the texel *is* the colour, so
substituting white turned every black fade into a white flash.

**Fix.** `MetalNative.inc` now reads `openfl_HasColorTransform` and only swaps in
the built-in white 1x1 when it is true.  All four combinations are then correct:
hct=1 x white multiplier -> white flash, hct=1 x black multiplier -> black,
hct=0 -> the engine's own texture decides.

## 2026-11-25: native ran at 34-56 fps where bgfx held 60

**Measurement.** `CNE_METAL4_PERF=1` reports the whole frame interval, the encode
segment, the pre-upload GPU wait (`cpuwrite`) and the mid-frame wait (`fbogpu`).
In the same scene:

```text
native  interval=17.8..29.3ms  encode=2.0..7.6ms  gap=15.7..25.8ms  cpuwrite=8.5..14.2ms
bgfx    interval=16.6..17.4ms  (stable, ~60 fps)
```

**Root cause.** `SplitFrameBeforeScreen()` submitted the FBO pass and then called
`WaitForGpu()` before encoding the screen pass.  Each frame therefore ran as
CPU(upload) -> GPU(FBO) -> CPU(screen encode) -> GPU(screen): the CPU blocked on
the FBO pass, then the GPU idled while the CPU encoded the screen pass.  That
bubble is the difference between 23 ms and bgfx's 16.6 ms.

**Fix.** The FBO pass and the screen pass are two command buffers committed to the
same queue, and a queue executes in submission order, so the FBO -> screen
dependency is already enforced without blocking the CPU.  The wait is now off by
default; `CNE_METAL4_SPLIT_WAIT=1` restores it.  `frame_begin` still waits for the
previous frame before resetting the scratch arena, and `WaitBeforeCpuWrite()`
still guards the single-buffered uploads, so the write-race protection is intact.

Result: A/B in the same run, same scene - wait on: 34-56 fps; wait off: normal
60 fps, no corruption.
