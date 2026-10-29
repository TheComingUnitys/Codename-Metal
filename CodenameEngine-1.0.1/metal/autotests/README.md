# Pixel regression (Metal backend)

Captures raw frames with `CNE_METAL_SHOT_RAW` and compares them with goldens.

## Quick start

```sh
# tool + diff self test (no app build needed)
CodenameEngine-1.0.1/metal/scripts/pixel-regression.sh --selftest

# capture the "menu" scenario; writes goldens the first time
CodenameEngine-1.0.1/metal/scripts/pixel-regression.sh \
  --app export/release/macos/bin/CodenameEngine.app --scenario menu --update

# later runs compare against the goldens (exit 1 on regression)
CodenameEngine-1.0.1/metal/scripts/pixel-regression.sh \
  --app export/release/macos/bin/CodenameEngine.app --scenario menu
```

## Scenarios

`scenarios/<name>.json`:

```json
{
  "autokeys": "120:40",                                  // optional synthetic input
  "captures": [{"frame": 240, "name": "stage"}],
  "tolerance": {"diffPixels": 4000, "mean": 2.0, "abs": 24}
}
```

- `autokeys` uses the `CNE_METAL_AUTOKEYS` format (`frame:key[:mods]`, comma separated).
- Each capture becomes `golden/<scenario>/<name>.{raw,txt}`.
- Goldens are **machine/driver specific**: regenerate with `--update` on the
  reference machine; do not expect them to match across GPUs.

Artifacts from a run (raw frames, `engine.log`, `*.diff.bmp`) are written to a
temp dir, or to `--outdir`.
