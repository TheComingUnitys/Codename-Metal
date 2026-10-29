# Diagnostic tools

Two standalone programs. They are not part of the engine build; they exist for render
debugging. **English** | [中文](../../../docs/zh-CN/metal/tools/README.md)

## winid.m - list windows / screenshot by window id

```sh
clang -fobjc-arc -framework Foundation -framework CoreGraphics -o winid winid.m
./winid              # all windows: id pid owner layer bounds title
./winid <pid>        # one process
./winid Codename     # filter by owner name
screencapture -x -l <CGWindowID> /tmp/shot.png
```

`screencapture -l` captures a fully occluded window and needs no accessibility permission:

```sh
PID=$(pgrep -f "CodenameEngine.app/Contents/MacOS/CodenameEngine" | head -1)
ID=$(./winid $PID | awk '$5 ~ /1249x/ {print $1}' | head -1)   # main window
screencapture -x -l "$ID" /tmp/game.png
```

Pitfall: `kCGWindowListOptionOnScreenOnly` often returns an **empty list** for an occluded
game window; the tool uses `kCGWindowListOptionAll` instead. The `0x0`/`1470x33`/`64x64`
entries are SDL/IME helper windows - pick the main window by bounds.

## cametaltest.m - environment capability check

Minimal NSWindow + CAMetalLayer that clears to **red** and presents every frame.

```sh
clang -fobjc-arc -framework Cocoa -framework Metal -framework QuartzCore -o cametaltest cametaltest.m
./cametaltest
```

- screen turns **red** -> CAMetalLayer works; the problem is engine-side;
- stays **black** -> environment/display-permission problem, stop suspecting the code.

Measured: full-screen red (~213,080 sample points), which settled the "can this machine
display Metal at all" question.

## In-engine switches (environment variables)

| Variable | Effect |
| --- | --- |
| `CNE_METAL_CLEAR_TEST=1` | force magenta clear (R==B, cannot reveal channel swap) |
| `CNE_METAL_CLEAR_TEST=RRGGBB` / `RRGGBBAA` | force a solid colour. **Use an asymmetric colour** (e.g. `FF0000`): red = present chain is correct, blue = swapped |
| `CNE_METAL_TEX_BGRA=1` | force every 8-bit upload to BGRA8 storage (diagnostic fallback) |
| `CNE_METAL_NOBLEND=1` | disable blending (separates "fully transparent" from "not drawn") |
| `CNE_METAL_DEBUG=1` | attribute layouts, program attributes, vertex dumps, MSL dump, `[cne-tex]` byte statistics |
| `CNE_METAL_STATS=1` | `bgfxDraws` / `glDrawArrays` / `glDrawElements` counters |
| `CNE_METAL_PRESENT_TEST=1` | blit+present a self-filled texture, bypassing bgfx render output |
| `CNE_METAL_SHOT=frame:path` | in-engine screenshot (backbuffer read; the bgfx/Metal path currently does not write files, use winid + screencapture) |

`[cne-tex]` example (`CNE_METAL_DEBUG=1`):

```text
[cne-tex] 1843x1396 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8
          opaque=65536 avgR=119.9 avgB=147.7 avgA=250.8  R>B in 12261
```

Compare `avgR/avgB` against the source PNG under raw / premultiplied /
premultiplied-with-R/B-swapped assumptions to identify the uploaded byte order.

## Is audio actually rendering? CoreAudio process object

macOS 14.2+ can answer "is this process rendering to an audio device"
(`/tmp/audioproc.m`):

```sh
clang -fobjc-arc -framework Foundation -framework CoreAudio -o audioproc audioproc.m
./audioproc <pid>        # without arguments: list all audio processes
```

```text
obj=186 pid=88067 runningInput=0 runningOutput=1 bundle=com.yoshman29.codenameengine
```

`runningOutput=1` means it is really producing sound. Control: `afplay
/System/Library/Sounds/Ping.aiff &` shows `runningOutput=1`, a silent process shows 0.

OpenAL Soft's own log reveals the backend:

```sh
ALSOFT_LOGLEVEL=3 ALSOFT_LOGFILE=/tmp/alsoft.log ./CodenameEngine
grep -E "Supported backends|Initialized backend|Created device" /tmp/alsoft.log
# expected: Supported backends: core, null / Created device ..., "MacBook Air Speakers"
```

`Supported backends: null` + `Created device ..., "No Output"` means all audio went nowhere.

## Synthetic keys: `CNE_METAL_AUTOKEYS` (no system permissions)

```sh
CNE_METAL_AUTOKEYS="1300:40,1700:81,1850:81,2050:40,2600:40" ./CodenameEngine
# frame:key,...   Return=40 Up=82 Down=81 Left=80 Right=79 Esc=41 Tab=43
```

Keys are injected with `SDL_PushEvent` into lime's own queue, bypassing the
"no accessibility permission means no external key injection" limit. **KEYDOWN and KEYUP
must be in different frames** (keep 4 frames), otherwise Flixel receives press+release
before one update and `justPressed` never fires.

Combine with `open -a CodenameEngine.app` (activate the window) and
`screencapture -x -l <windowID>` (captures even when the harness window is on top).

## View ordering A/B: `CNE_METAL_VIEWMODE`

```sh
CNE_METAL_VIEWMODE=default ./CodenameEngine     # bgfx default (sort by program/blend)
CNE_METAL_VIEWMODE=depth   ./CodenameEngine     # DepthAscending
# unset = Sequential (submission order, what the 2D renderer needs)
```

The chosen mode is printed at startup: `[cne-metal] view mode = 1 (env=unset)`.

## Draw-order probe

With `CNE_METAL_DEBUG=1` every draw prints frame, index and first vertex position:

```text
[cne-dbg] f=2555 draw#11 stream 0 stride=20 bytes=224 base=0 pos=(95.0,65.0) layout: ...
```

## Build-time tool: `CompileShaderAot.mm` - MSL -> `.metallib`

```sh
# normally automatic: apply.sh -> gen_shader_aot.py builds and calls it
python3 metal/scripts/gen_shader_aot.py            # incremental

clang++ -x objective-c++ -fobjc-arc -std=c++17 -O2 \
  metal/tools/CompileShaderAot.mm -framework Metal -framework Foundation \
  -o metal/tools/build/CompileShaderAot
metal/tools/build/CompileShaderAot <manifest.json> <outdir> [--force]
```

- no full Xcode / `xcrun metal` here, so it drives the system Metal compiler
  (`newLibraryWithSource` in the build process) and serialises the `MTLLibrary` with the
  private `serializeToURL:error:` into a real `.metallib` (`file` reports
  `MetalLib executable`). The game only uses the public `newLibraryWithData`.
- every artefact is re-loaded and verified immediately
  (`newLibraryWithData` + `newFunctionWithName:@"xlatMtlMain"`).
- incremental: `<key>.meta` stores MSL fingerprint + device/OS tag + byte size; reused only
  when all three match (new machine/OS recompiles automatically).
- if the private API is unavailable (future macOS), exit code 4 makes the generator fall
  back to "MSL only" and the build continues.
- a stage whose MSL does not compile is skipped (runtime falls back to
  `newLibraryWithSource`) without failing the build.

Output: `metal/generated/metallibs/<macOS version>_<build>_<arch>/` (local build cache).
