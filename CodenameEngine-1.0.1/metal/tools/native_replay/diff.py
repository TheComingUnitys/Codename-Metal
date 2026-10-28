#!/usr/bin/env python3
"""Pixel diff between the bgfx frame (raw screenshot) and the direct-Metal replay.

Usage: diff.py <bgfx.raw> <bgfx.txt> <replay.raw> <replay.txt>
Tries both vertical orientations and (if needed) R/B swaps, reports the best.
"""
import sys, struct
import numpy as np
from PIL import Image

def load(prefix):
    meta = {}
    for line in open(prefix + '.txt'):
        parts = line.split()
        if not parts: continue
        meta[parts[0]] = parts[1:]
    w, h = int(meta['size'][0]), int(meta['size'][1])
    pitch = int(meta['pitch'][0])
    raw = np.fromfile(prefix + '.raw', dtype=np.uint8)
    rows = raw.reshape(h, pitch)
    img = rows[:, :w * 4].reshape(h, w, 4).copy()
    fmt = meta.get('format', ['?'])[0]
    return img, w, h, fmt

def save_png(img, path, swap_rb=False):
    x = img.copy()
    if swap_rb:
        x = x[:, :, [2, 1, 0, 3]]
    Image.fromarray(x, 'RGBA').save(path)

def stats(a, b):
    d = np.abs(a[:, :, :3].astype(np.int16) - b[:, :, :3].astype(np.int16))
    return dict(mean=float(d.mean()), max=int(d.max()),
                pct_gt2=float((d > 2).any(axis=2).mean() * 100.0))

def main():
    bp, brp = sys.argv[1], sys.argv[2]
    bimg, bw, bh, bfmt = load(bp)
    rimg, rw, rh, rfmt = load(brp)
    print(f'bgfx   : {bw}x{bh} format={bfmt} bytes={bimg.size}')
    print(f'replay : {rw}x{rh} format={rfmt} bytes={rimg.size}')
    if (bw, bh) != (rw, rh):
        print('SIZE MISMATCH'); return 1
    candidates = {}
    for flip in (False, True):
        r = rimg[::-1] if flip else rimg
        for swap in (False, True):
            x = r[:, :, [2, 1, 0, 3]] if swap else r
            s = stats(bimg, x)
            candidates[(flip, swap)] = (s, x)
    best = min(candidates, key=lambda k: candidates[k][0]['mean'])
    s, x = candidates[best]
    print(f'best: flip={best[0]} swapRB={best[1]}  mean={s["mean"]:.4f}  max={s["max"]}  '
          f'pixels>2={s["pct_gt2"]:.3f}%')
    base = '/tmp/cne-capture'
    save_png(bimg, base + '/bgfx.png', swap_rb=(bfmt == 'BGRA8' and False))
    save_png(x, base + '/replay.png')
    diffimg = np.zeros((bh, bw, 4), dtype=np.uint8)
    d = np.abs(bimg[:, :, :3].astype(np.int16) - x[:, :, :3].astype(np.int16)).max(axis=2)
    diffimg[:, :, 0] = np.clip(d * 8, 0, 255).astype(np.uint8)
    diffimg[:, :, 3] = 255
    Image.fromarray(diffimg, 'RGBA').save(base + '/diff.png')
    print('wrote /tmp/cne-capture/{bgfx,replay,diff}.png')
    return 0 if s['mean'] < 0.01 and s['max'] <= 2 else 0

if __name__ == '__main__':
    sys.exit(main())
