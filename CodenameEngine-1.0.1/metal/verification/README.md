# Verification screenshots (round 49)

**English** | [中文](../../../docs/zh-CN/metal/verification/README.md)

All screenshots were captured with `screencapture -x -l <CGWindowID>` (works even when the
window is covered by the harness UI); window ids come from `metal/tools/winid.m`.

| File | Content | Notes |
| --- | --- | --- |
| `01-before-RB-swapped.png` | before the fix | R/B swapped: FNF logo **purple with orange outline**, GF hair **blue**, title text **yellow** |
| `02-after-RB-fixed.png` | after the fix (release build) | logo **magenta with blue outline**, GF hair **auburn**, dress **red**, title **cyan** - matches the PNGs in `assets/` channel by channel |
| `03-clear-test-red.png` | `CNE_METAL_CLEAR_TEST=FF0000` | the forced clear colour at the screen edge **is red**, so the present chain (bgfx swap chain -> CAMetalLayer) does **not** swap channels; the swap can only come from the upload format tag |

## Why the red clear screen matters

Magenta (`0xFF00FF`) has R == B, so a channel swap is **invisible**; the old
`CNE_METAL_CLEAR_TEST=1` could not answer the question. Use an **asymmetric** colour:

```sh
CNE_METAL_CLEAR_TEST=FF0000 ./CodenameEngine.app/Contents/MacOS/CodenameEngine
# red screen -> present chain correct; blue screen -> present chain swaps channels
```

The log echoes the colour actually used:

```text
[cne-metal] clear test: rgba=0xFF0000FF (env="FF0000")
```

## Reproducing without eyeballing

With `CNE_METAL_DEBUG=1` every texture upload prints runtime byte statistics:

```text
[cne-tex] 1843x1396 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8
          opaque=65536 avgR=119.9 avgB=147.7 avgA=250.8  R>B in 12261
```

Compare `avgR/avgB` with the source PNG under three hypotheses:

| Texture | Runtime | Source raw | Source premultiplied | Source pre-mul + R/B swapped |
| --- | --- | --- | --- | --- |
| `logo.png` | **119.9 / 147.7** | 148.3 / 122.5 | 147.7 / 119.9 | **119.9 / 147.7** (match) |
| `gf.png` | **12.8 / 107.0** | 107.3 / 13.5 | 107.0 / 12.8 | **12.8 / 107.0** (match) |

After the fix `format` becomes `0x80E1` (`GL_BGRA_EXT`) and `-> BGRA8` is the correct mapping.
