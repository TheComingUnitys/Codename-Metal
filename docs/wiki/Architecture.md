# Architecture

## Layers

```text
Codename Engine (Haxe/Flixel)
  → openfl-metal (OpenFL Metal fork: renderer + instancing batch)
  → flixel-metal (Flixel fork: FlxG.metalInstancing)
  → lime 8.3.2 + Metal backend (metal/native: GL-shaped entry points)
  → bgfx Metal renderer
  → Metal / CAMetalLayer
```

## Metal backend

The backend lives in `CodenameEngine-1.0.1/metal/native`. It exposes the Lime/OpenFL GL-shaped API (`lime_gl_*`) and translates each call to bgfx:

- GL objects (textures, buffers, framebuffers, programs, VAOs) are cached and mapped to bgfx handles.
- Frame passes are grouped into bgfx views; `ApplyFrameViewOrder` builds a dependency-ordered view list.
- Render targets are bgfx framebuffers; textures are promoted to `BGFX_TEXTURE_RT` once when attached.
- The full backbuffer is cleared once per frame via a dedicated view.

## Shaders

- The Haxe GLSL→MSL translator lives in `metal/haxe/lime/graphics/metal/GLSLToMSL.hx`.
- At build time every engine GLSL shader is translated to MSL and compiled to `.metallib`; the Haxe cache (`ShaderAotCache.hx`) is generated.
- At runtime the shader source is hashed and looked up in the cache: no translation for the engine corpus.
- Mod shaders are translated at runtime and memoized in-process.

## Metal OpenFL/Flixel forks

- **openfl-metal** adds the Metal native-path shims and `InstancedDisplayObjectShader` + `MetalInstanceBatch` (opt-in instanced bitmap batching).
- **flixel-metal** adds `FlxG.metalInstancing` (default off).

## Universal binary

`metal/scripts/make-universal.sh` builds arm64 and x86_64, lipo-combines the executable and `lime.ndll`, signs ad-hoc and packages the DMG.
