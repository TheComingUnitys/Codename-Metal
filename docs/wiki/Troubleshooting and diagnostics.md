# Troubleshooting and diagnostics

## Run from Terminal to see logs

```sh
/Applications/CodenameEngine.app/Contents/MacOS/CodenameEngine
```

## Common issues

### Gatekeeper blocks the app
The build is ad-hoc signed, not notarized:

```sh
xattr -dr com.apple.quarantine /Applications/CodenameEngine.app
```

or right-click → Open.

### Black screen / missing world
- Try `CNE_METAL_LEGACY=1` to disable the draw-path caches.
- Try `CNE_METAL_SHARED_DEPTH=0` (the default) — if it was enabled, disable it.
- Try `CNE_METAL_RT_TEXTURES=1` to restore the legacy all-render-target texture behavior.
- Collect the log and open an issue with the last 50 lines.

### Repeated / tiled sprites or text
- Try `CNE_METAL_INSTANCING=0` (default) and `CNE_METAL_FAST_SAMPLER=1` is **not** default.
- Check that you are not mixing a stale app with a new ndll.

### Window / resize glitches
- The app does not restore the previous window frame (`window.restorable = NO`).
- Report the window size and scale from the log's `[cne-metal] hostView=...` line.

## Diagnostics

```sh
CNE_METAL_STATS=1       # draw counters every 120 frames
CNE_METAL_PROFILE=1     # CPU front-end cost
CNE_METAL_HITCH=1       # frame hitches
CNE_METAL_TRACE=1       # draw/frame trace
CNE_METAL_LEGACY=1      # disable A/C fast paths
```

Shader AOT: the exit line reports `cache=... entries=... hits=... misses=...`; misses mean a shader was not AOT-compiled.

## Reporting

Please include:
- macOS version and Mac model (Intel/Apple Silicon),
- the app version / DMG SHA-256,
- the terminal log (last 50-100 lines),
- steps to reproduce and the affected mod/song.
