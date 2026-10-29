#!/usr/bin/env python3
"""Compares CNE_METAL_SHOT_RAW frames and reports / images the differences.

The Metal backend can dump a frame with

    CNE_METAL_SHOT_RAW="120:/tmp/menu" CodenameEngine

which writes /tmp/menu.raw (headerless BGRA/RGBA, 4 bytes per pixel) plus
/tmp/menu.txt ("size W H", "pitch", "format").  This tool compares two of those
dumps and is used by metal/scripts/pixel-regression.sh.

Exit codes: 0 frames match, 1 regression, 2 usage/IO error.

Run frame_diff.py --selftest for the dependency-free self test (used by verify.sh).
"""

import argparse
import json
import os
import struct
import sys
import tempfile

CHANNELS = {
    'bgra': (2, 1, 0),
    'rgba': (0, 1, 2),
}


class Frame(object):
    def __init__(self, width, height, pitch, fmt, raw, path):
        self.width = width
        self.height = height
        self.pitch = pitch
        self.format = fmt
        self.raw = raw
        self.path = path

    def rgb(self, x, y, order):
        off = y * self.pitch + x * 4
        r, g, b = order
        return self.raw[off + r], self.raw[off + g], self.raw[off + b]


def load_frame(path):
    meta_path = path if path.endswith('.txt') else path + '.txt'
    raw_path = path[:-4] + '.raw' if path.endswith('.txt') else path + '.raw'
    try:
        with open(meta_path, 'r', encoding='utf-8') as fh:
            meta = fh.read()
        with open(raw_path, 'rb') as fh:
            raw = fh.read()
    except OSError as exc:
        raise SystemExit('error: cannot read frame %s: %s' % (path, exc))

    width = height = pitch = fmt = None
    for line in meta.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == 'size':
            width, height = int(parts[1]), int(parts[2])
        elif len(parts) == 2 and parts[0] == 'pitch':
            pitch = int(parts[1])
        elif len(parts) == 2 and parts[0] == 'format':
            fmt = int(parts[1])
    if not width or not height or not pitch:
        raise SystemExit('error: %s is missing size/pitch' % meta_path)
    if len(raw) < pitch * height:
        raise SystemExit('error: %s holds %d bytes, expected %d'
                         % (raw_path, len(raw), pitch * height))
    return Frame(width, height, pitch, fmt, raw, raw_path)


def scaled(frame, width, height):
    """Nearest-neighbour rescale, used when two captures have different drawable sizes."""
    raw = bytearray(width * height * 4)
    for y in range(height):
        sy = min(frame.height - 1, (y * frame.height) // height)
        for x in range(width):
            sx = min(frame.width - 1, (x * frame.width) // width)
            src = sy * frame.pitch + sx * 4
            dst = (y * width + x) * 4
            raw[dst:dst + 4] = frame.raw[src:src + 4]
    return Frame(width, height, width * 4, frame.format, bytes(raw), frame.path)


def compare(a, b, order, tolerance, allow_size_mismatch=False):
    if a.width != b.width or a.height != b.height:
        if not allow_size_mismatch:
            raise SystemExit('error: frame sizes differ: %dx%d vs %dx%d'
                             % (a.width, a.height, b.width, b.height))
        width = min(a.width, b.width)
        height = min(a.height, b.height)
        a = scaled(a, width, height)
        b = scaled(b, width, height)

    diff_pixels = 0
    total_abs = 0
    max_abs = 0
    bbox = None
    for y in range(a.height):
        for x in range(a.width):
            pa = a.rgb(x, y, order)
            pb = b.rgb(x, y, order)
            delta = max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
            if delta == 0:
                continue
            total_abs += delta
            max_abs = max(max_abs, delta)
            if delta <= tolerance:
                continue
            diff_pixels += 1
            if bbox is None:
                bbox = [x, y, x, y]
            else:
                bbox[0] = min(bbox[0], x)
                bbox[1] = min(bbox[1], y)
                bbox[2] = max(bbox[2], x)
                bbox[3] = max(bbox[3], y)

    pixels = a.width * a.height
    return {
        'width': a.width,
        'height': a.height,
        'pixels': pixels,
        'diffPixels': diff_pixels,
        'meanAbs': (total_abs / float(pixels)) if pixels else 0.0,
        'maxAbs': max_abs,
        'bbox': bbox,
    }


def write_bmp(path, a, b, order, tolerance):
    """Writes a 24-bit BMP: the reference frame dimmed, differing pixels in red."""
    row_bytes = (a.width * 3 + 3) & ~3
    pixels = bytearray()
    for y in range(a.height - 1, -1, -1):
        row = bytearray()
        for x in range(a.width):
            pa = a.rgb(x, y, order)
            if b is not None:
                pb = b.rgb(x, y, order)
                delta = max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
                if delta > tolerance:
                    row += bytes((0, 0, 255))          # BGR red
                    continue
            row += bytes((pa[2] // 2, pa[1] // 2, pa[0] // 2))
        row += b'\x00' * (row_bytes - len(row))
        pixels += row

    header = struct.pack('<2sIHHI', b'BM', 14 + 40 + len(pixels), 0, 0, 14 + 40)
    info = struct.pack('<IiiHHIIiiII', 40, a.width, a.height, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, 'wb') as fh:
        fh.write(header)
        fh.write(info)
        fh.write(pixels)


def judge(stats, args):
    problems = []
    if args.max_diff_pixels is not None and stats['diffPixels'] > args.max_diff_pixels:
        problems.append('diffPixels %d > %d' % (stats['diffPixels'], args.max_diff_pixels))
    if args.max_mean is not None and stats['meanAbs'] > args.max_mean:
        problems.append('meanAbs %.3f > %.3f' % (stats['meanAbs'], args.max_mean))
    if args.max_abs is not None and stats['maxAbs'] > args.max_abs:
        problems.append('maxAbs %d > %d' % (stats['maxAbs'], args.max_abs))
    return problems


def run_selftest():
    """Synthetic frames: identical (pass), one pixel (tolerance dependent), shifted (fail)."""
    td = tempfile.mkdtemp(prefix='cne-frame-diff-')
    w = h = 8

    def make(name, shift):
        rows = bytearray()
        for y in range(h):
            for x in range(w):
                v = 255 if ((x + shift) % 2 == 0) else (x * 8 + y)
                rows += bytes((v, v, v, 255))   # BGRA, opaque
        raw = os.path.join(td, name + '.raw')
        with open(raw, 'wb') as fh:
            fh.write(rows)
        with open(os.path.join(td, name + '.txt'), 'w', encoding='utf-8') as fh:
            fh.write('size %d %d\npitch %d\nformat 73\nbytes %d\n' % (w, h, w * 4, len(rows)))
        return os.path.join(td, name)

    ok = True
    base = load_frame(make('base', 0))
    same = load_frame(make('same', 0))
    shifted = load_frame(make('shifted', 1))

    st = compare(base, same, CHANNELS['bgra'], 0)
    if st['diffPixels'] != 0:
        print('selftest FAIL: identical frames differ (%d px)' % st['diffPixels'])
        ok = False
    else:
        print('selftest ok: identical frames -> 0 diff pixels')

    st = compare(base, shifted, CHANNELS['bgra'], 0)
    if st['diffPixels'] <= 0 or st['maxAbs'] <= 0 or st['bbox'] is None:
        print('selftest FAIL: shifted frame reported as identical')
        ok = False
    else:
        print('selftest ok: shifted frame -> %d diff pixels, bbox %s'
              % (st['diffPixels'], st['bbox']))

    class Args(object):
        max_diff_pixels = 0
        max_mean = 0.0
        max_abs = 0

    if not judge(st, Args):
        print('selftest FAIL: threshold judge accepted a shifted frame')
        ok = False
    else:
        print('selftest ok: thresholds reject a shifted frame')

    bmp = os.path.join(td, 'diff.bmp')
    write_bmp(bmp, base, shifted, CHANNELS['bgra'], 0)
    if not (os.path.isfile(bmp) and os.path.getsize(bmp) > 54):
        print('selftest FAIL: diff image not written')
        ok = False
    else:
        print('selftest ok: diff image written (%d bytes)' % os.path.getsize(bmp))

    print('frame_diff selftest: %s' % ('PASSED' if ok else 'FAILED'))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference', nargs='?', help='golden frame (path without .raw)')
    parser.add_argument('actual', nargs='?', help='captured frame (path without .raw)')
    parser.add_argument('--tolerance', type=int, default=0,
                        help='per-channel difference ignored when counting diff pixels')
    parser.add_argument('--max-diff-pixels', type=int, default=None)
    parser.add_argument('--max-mean', type=float, default=None)
    parser.add_argument('--max-abs', type=int, default=None)
    parser.add_argument('--channel-order', choices=sorted(CHANNELS), default='bgra')
    parser.add_argument('--allow-size-mismatch', action='store_true',
                        help='rescale both frames to the smaller size before comparing')
    parser.add_argument('--diff-image', default=None, help='write a BMP visualising the diff')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if not args.reference or not args.actual:
        parser.error('reference and actual are required (or use --selftest)')

    order = CHANNELS[args.channel_order]
    ref = load_frame(args.reference)
    act = load_frame(args.actual)
    stats = compare(ref, act, order, args.tolerance, args.allow_size_mismatch)
    problems = judge(stats, args)

    if args.diff_image:
        write_bmp(args.diff_image, ref, act, order, args.tolerance)

    if args.json:
        print(json.dumps({'reference': args.reference, 'actual': args.actual,
                          'stats': stats, 'problems': problems}, sort_keys=True))
    else:
        print('[frame-diff] %s vs %s: %dx%d, diffPixels=%d meanAbs=%.3f maxAbs=%d bbox=%s'
              % (args.reference, args.actual, stats['width'], stats['height'],
                 stats['diffPixels'], stats['meanAbs'], stats['maxAbs'], stats['bbox']))

    if problems:
        for problem in problems:
            sys.stderr.write('[frame-diff] regression: %s\n' % problem)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
