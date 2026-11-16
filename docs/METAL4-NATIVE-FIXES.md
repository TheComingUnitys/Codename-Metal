# Metal 4 native draw — black-screen diagnosis and fix (2026-11-16)

Status: fixes are built into `~/Downloads/CodenameEngine-arm64-m4native.app` and the
default bgfx path is untouched. The native path still needs a human play-test.

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
