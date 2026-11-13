# Codename Engine 1.0.1 — Metal Backend

A macOS Metal backend for [Codename Engine 1.0.1](https://github.com/CodenameCrew/CodenameEngine), built on bgfx + Lime + Metal-specific OpenFL/Flixel forks.

- Repository: https://github.com/TheComingUnitys/Codename-Metal
- Latest release: [v1.0.1-metal.5](https://github.com/TheComingUnitys/Codename-Metal/releases/tag/v1.0.1-metal.5)
- Engine base: Codename Engine **1.0.1**
- Platforms: macOS 11+ (arm64 + x86_64 universal)

## Quick start

1. Download the latest universal DMG from the Releases page.
2. Open the DMG and drag `CodenameEngine.app` to Applications (or run it from the mounted image).
3. If macOS blocks the ad-hoc signed build, right-click the app → Open (or `xattr -dr com.apple.quarantine` on the app bundle).
4. Run the game. Mods live next to the app's `mods/` directory as usual.

The build is **not notarized**; Gatekeeper warnings are expected.

## What is different from the upstream engine?

- The renderer is Metal (bgfx Metal backend), not OpenGL.
- GLSL shaders are precompiled to Metal Shading Language and `.metallib` at build time (AOT); mod shaders are translated at runtime.
- Two Metal-specific forks are used:
  - [openfl-metal](https://github.com/TheComingUnitys/openfl-metal)
  - [flixel-metal](https://github.com/TheComingUnitys/flixel-metal)
- The backend includes draw-path caching, streaming textures, draw merging and optional instanced bitmap batching.

## Wiki pages

- [[Installation]]
- [[Building from source]]
- [[Architecture]]
- [[Performance optimizations]]
- [[Troubleshooting and diagnostics]]
- [[Dependencies and versions]]
- [[Release history]]

## Links

- Upstream engine: https://github.com/CodenameCrew/CodenameEngine
- Engine docs: https://codename-engine.com/
- Metal renderer core: bgfx (Metal) + Lime 8.3.2
