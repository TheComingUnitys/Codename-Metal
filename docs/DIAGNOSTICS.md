# Metal backend diagnostics & testing manual

**English** | [中文](zh-CN/DIAGNOSTICS.md)

This page turns the scattered environment variables into one **unified entry point** and
explains how to read each output. All legacy variables keep working (the script only maps
categories onto them), so existing launch scripts need no changes.

One-shot verification (no game run): `./verify.sh`; pixel regression:
`metal/autotests/README.md`.

> Commands below run from `CodenameEngine-1.0.1/` (`diag.sh` lives in `metal/scripts/`).

---

## 1. Unified entry point: `metal/scripts/diag.sh`

```sh
./metal/scripts/diag.sh --list

./metal/scripts/diag.sh --print stats,fbo

./metal/scripts/diag.sh -c stats,profile,hitch -- <app>/Contents/MacOS/CodenameEngine

# screenshot at frame 240 and capture frame 300 at the same time
./metal/scripts/diag.sh --shot 240:/tmp/stage --capture-frame 300 --capture-dir /tmp/cap \
  -- <app>/Contents/MacOS/CodenameEngine

# synthetic keys (frame:SDL scancode[:mods])
./metal/scripts/diag.sh --keys "120:40,180:40" -- <app>/Contents/MacOS/CodenameEngine
```

| Category | Environment variable | Output / purpose |
| --- | --- | --- |
| `stats` | `CNE_METAL_STATS=1` | every 120 frames: present/frame, bgfxDraws, glDrawElements, submitDraw, backbuffer |
| `profile` | `CNE_METAL_PROFILE=1` | every 240 frames: CPU front-end cost (bind/submit/viewAlloc/flip/view/uniform/bgfxSubmit) + texture uploads |
| `hitch` | `CNE_METAL_HITCH=1` | frames > 34 ms plus tex/fbo/program creation counts |
| `hitch-detail` | `CNE_METAL_HITCH_DETAIL=1` | split a slow frame into game/submit/present/rest |
| `draws` | `CNE_METAL_DRAWPROBE=1` | per draw: program/fbo/tex/size/rt/m0/m5/m13 |
| `fbo` | `CNE_METAL_FBOLOG=1` | framebuffer create/invalidate log |
| `uv` | `CNE_METAL_UVLOG=1` | screen composite flip decision |
| `tex` | `CNE_METAL_TEXLOG=1` | texture creation sizes (thumbnail-render bugs) |
| `coverage` | `CNE_METAL_COVERAGE=1` | `lime_gl_*` call coverage; output path via `CNE_METAL_COVERAGE_OUT` |
| `shader` | `CNE_METAL_SHADER_AUDIT=1` | GLSL->MSL audit log (`CNE_METAL_SHADER_AUDIT_FRAME` limits the frame) |
| `aot` | `CNE_METAL_AOT_AUDIT=1` | AOT lookup summary (hits/misses/runtimeTranslations) |
| `mtl` | `CNE_METAL_MTL_AUDIT=1` | Metal shader/program load + compile statistics |
| `debug` | `CNE_METAL_DEBUG=1` | general backend debug output |

Special parameters:

- `--shot <frame>:<path>[,<frame>:<path>...]` -> `CNE_METAL_SHOT_RAW` (multi-frame; pixel regression);
- `--capture-frame N` / `--capture-dir DIR` -> `CNE_METAL_CAPTURE_FRAME` / `CNE_METAL_CAPTURE_DIR`;
- `--keys <spec>` -> `CNE_METAL_AUTOKEYS` (`frame:scancode[:mods]`; 40=Return, 82/81=Up/Down).

---

## 2. Output formats

### 2.1 Frame statistics (`stats`)

```text
[cne-stats] present=961 frame=961 bgfxDraws=9 glDrawArrays=0 glDrawElements=14899 submitDraw=14899 backbuffer=2498x1440
```

- `bgfxDraws`: bgfx submits this frame; `glDrawElements/glDrawArrays`: GL-shaped calls
  (should be close to `submitDraw`); `backbuffer`: current render-target size.

### 2.2 CPU front-end cost (`profile`)

```text
[cne-prof] f=240 draws/frame=48.0 frontEnd=812.3us/frame [bind=... submitAll=... viewAlloc=... flip=... view=... uniformApply=... bgfxSubmit=... misc=...] uniform=... texUpload=... texMB=0.12/frame
```

Threshold regression: `python3 metal/tools/profile_check.py <log> --max-frontend-us 1500 --max-hitch-ms 60`
(also `--max-draws`; `--selftest` needs no app).

### 2.3 Hitches (`hitch` / `hitch-detail`)

```text
[cne-hitch] frame=540 dt=210.4ms texCreates=12 fboCreates=3 programsLinked=4 now=12000ms
[cne-hitch] summary frame=7200 worstFrame=210.4ms
```

### 2.4 Shaders / AOT (`aot` + `mtl` + `shader`)

```text
[cne-aot] EXIT cache=... entries=116 metallibStages=114 hits=... misses=0 runtimeTranslations=0
[cne-mtl] EXIT shaders=44 metallib=42 sourceCompile=2 metallibLoadFailures=0 ... newLibraryWithSourceMs=...
```

`sourceCompile>0` means some stage fell back to runtime MSL compilation (not covered by AOT);
these must be listed in `metal/audit/aot-expected.json`. Anything outside the allowlist makes
the strict audit in `./verify.sh` fail.

### 2.5 Screenshots / raw frames (`--shot`)

Writes `<path>.raw` (headerless, 4 bytes/px, BGRA) plus `<path>.txt` (size/pitch/format/bytes).
Compare with `metal/tools/frame_diff.py`; scenario regression:
`metal/scripts/pixel-regression.sh --selftest`.

---

## 3. Frame capture + direct-Metal replay (A/B prototype)

```sh
CNE_METAL_CAPTURE_FRAME=240 CNE_METAL_CAPTURE_DIR=/tmp/cne-capture \
  ./metal/scripts/diag.sh --shot 240:/tmp/cne-shot/frame -- <app>/Contents/MacOS/CodenameEngine

./metal/tools/native_replay/build.sh /tmp/native_replay

/tmp/native_replay /tmp/cne-capture/f240 /tmp/replay
python3 ./metal/tools/native_replay/diff.py /tmp/cne-shot/frame.raw /tmp/replay.raw
```

The capture contains per-draw state, MSL, vertex/index bytes, uniforms and textures; the
replayer uses neither bgfx nor lime_gl. It is also the prerequisite for "drop bgfx".

---

## 4. Advanced / experimental switches (may change rendering semantics)

| Switch | Effect |
| --- | --- |
| `CNE_METAL_NOBLEND=1` | disable blending (separates "alpha is 0" from "not drawn") |
| `CNE_METAL_TEX_UPDATE=1` | experiment: stream textures through `updateTexture2D` (once caused a black screen) |
| `CNE_METAL_TEX_BGRA=1` | force BGRA storage for RGBA uploads |
| `CNE_METAL_FORCE_SOURCE_COMPILE=1` | skip metallibs, compile at runtime |
| `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` | disable the runtime GLSL translation fallback |
| `CNE_METAL_STATE_RESET=1` | one-shot state reset when a new FBO first appears |
| `CNE_METAL_SKIP_PROGRAM=<ids>` / `SKIP_DRAW` / `SKIP_VIEW` | skip specific draws/programs/views |
| `CNE_METAL_DUMP_WORLD` / `SHOW_WORLD_RT` / `SHOW_WORLDCORNER` | world-RT debug display |
| `CNE_METAL_UVLOG` / `FBO_FLIP` / `COMPOSITE_FLIP` / `FLIP_COMPOSITE` | flip-decision experiments (auto-decided in the normal path) |

### 4.1 GL states bgfx cannot express (decision: not implemented)

They are still **recorded in `MetalContext::state` and warned about once**
(`ReportUnimplemented`), never silently ignored:

| GL state | Reason / current behaviour |
| --- | --- |
| `glBlendColor` | bgfx v167 has no blend-colour setter; constant factors fall back |
| `glDepthRange` | no depth-range parameter; fixed [0,1] |
| `glPolygonOffset` | no depth-bias state |
| `glLineWidth` (>1) | Metal/bgfx rasterise 1px lines |
| MSAA / `glSampleCoverage` | MSAA disabled, coverage always full |
| nested-mask stencil write-mask | bgfx v167 stencil word has no write-mask field |
| unknown client pixel format | falls back to RGBA8 / 4 B/px with a warning (narrow formats are expanded) |
| offscreen view id > 199/frame | reuses view 199 and warns once |

Full list (~70):

```sh
grep -rho 'CNE_METAL_[A-Z0-9_]*' CodenameEngine-1.0.1/metal | sort -u
```

When adding a diagnostic: extend the `diag.sh` category table and document it here first.

---

## 5. Common combinations

| Goal | Command |
| --- | --- |
| frame rate / draw counts | `diag.sh -c stats -- <app>` |
| hitches / menu stalls | `diag.sh -c hitch,hitch-detail -- <app>` |
| are all shaders AOT'd | `diag.sh -c aot,mtl -- <app>` |
| black screen / orientation | `diag.sh -c uv,fbo,draws -- <app>` |
| AOT coverage regression | `./verify.sh` (`gen_shader_aot.py --check --strict`) |
| pixel regression | `metal/scripts/pixel-regression.sh --selftest` / autotests README |
