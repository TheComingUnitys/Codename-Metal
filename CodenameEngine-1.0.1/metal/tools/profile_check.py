#!/usr/bin/env python3
"""Parses CNE_METAL_PROFILE / CNE_METAL_HITCH logs and enforces thresholds.

    CNE_METAL_PROFILE=1 CNE_METAL_HITCH=1 <app> 2>&1 | tee /tmp/run.log
    python3 metal/tools/profile_check.py /tmp/run.log \
        --max-frontend-us 1500 --max-hitch-ms 60

Profile lines (one every 240 frames):

    [cne-prof] f=240 draws/frame=48.0 frontEnd=812.3us/frame
        [bind=... submitAll=... viewAlloc=... flip=... view=... uniformApply=...
         bgfxSubmit=... misc=...] uniform=...us/frame texUpload=...us/frame texMB=0.12/frame

Hitch lines:

    [cne-hitch] frame=540 dt=210.4ms texCreates=12 fboCreates=3 programsLinked=4 ...
    [cne-hitch] summary frame=7200 worstFrame=210.4ms

Exit codes: 0 within thresholds, 1 regression, 2 no data / IO error.
Run --selftest for the synthetic self test (no app needed; used by verify.sh).
"""

import argparse
import json
import os
import re
import sys
import tempfile

PROF_RE = re.compile(
    r'\[cne-prof\]\s+f=(\d+)\s+draws/frame=([\d.]+)\s+frontEnd=([\d.]+)us/frame')
HITCH_RE = re.compile(r'\[cne-hitch\]\s+frame=(\d+)\s+dt=([\d.]+)ms')


def parse(path):
    profile = []
    hitches = []
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                m = PROF_RE.search(line)
                if m:
                    profile.append({'frame': int(m.group(1)),
                                    'draws': float(m.group(2)),
                                    'frontEndUs': float(m.group(3))})
                    continue
                m = HITCH_RE.search(line)
                if m:
                    hitches.append({'frame': int(m.group(1)), 'dtMs': float(m.group(2))})
    except OSError as exc:
        raise SystemExit('error: cannot read %s: %s' % (path, exc))
    return profile, hitches


def summarise(profile, hitches):
    stats = {
        'profileSamples': len(profile),
        'hitchSamples': len(hitches),
    }
    if profile:
        stats['maxFrontEndUs'] = max(p['frontEndUs'] for p in profile)
        stats['meanFrontEndUs'] = sum(p['frontEndUs'] for p in profile) / len(profile)
        stats['maxDrawsPerFrame'] = max(p['draws'] for p in profile)
    if hitches:
        stats['maxHitchMs'] = max(h['dtMs'] for h in hitches)
    return stats


def judge(stats, args):
    problems = []
    if args.max_frontend_us is not None:
        value = stats.get('maxFrontEndUs')
        if value is None:
            problems.append('no [cne-prof] lines (was CNE_METAL_PROFILE=1 set?)')
        elif value > args.max_frontend_us:
            problems.append('maxFrontEndUs %.1f > %.1f' % (value, args.max_frontend_us))
    if args.max_draws is not None:
        value = stats.get('maxDrawsPerFrame')
        if value is None:
            problems.append('no [cne-prof] lines for draws/frame')
        elif value > args.max_draws:
            problems.append('maxDrawsPerFrame %.1f > %.1f' % (value, args.max_draws))
    if args.max_hitch_ms is not None:
        value = stats.get('maxHitchMs')
        if value is None:
            problems.append('no [cne-hitch] lines (was CNE_METAL_HITCH=1 set?)')
        elif value > args.max_hitch_ms:
            problems.append('maxHitchMs %.1f > %.1f' % (value, args.max_hitch_ms))
    return problems


def run_selftest():
    log = '''
[cne-prof] f=240 draws/frame=48.0 frontEnd=812.3us/frame [bind=1.0 submitAll=2.0] uniform=3.0us/frame texUpload=0.4us/frame texMB=0.12/frame
[cne-prof] f=480 draws/frame=51.0 frontEnd=910.0us/frame [bind=1.0 submitAll=2.0] uniform=3.0us/frame texUpload=0.4us/frame texMB=0.10/frame
[cne-hitch] frame=540 dt=41.2ms texCreates=12 fboCreates=3 programsLinked=4 now=12000ms
[cne-hitch] frame=900 dt=70.5ms texCreates=0 fboCreates=0 programsLinked=0 now=20000ms
'''
    td = tempfile.mkdtemp(prefix='cne-profile-')
    path = os.path.join(td, 'run.log')
    with open(path, 'w', encoding='utf-8') as fh:
        fh.write(log)

    profile, hitches = parse(path)
    stats = summarise(profile, hitches)
    ok = True
    if len(profile) != 2 or len(hitches) != 2:
        print('selftest FAIL: parsed %d profile / %d hitch lines' % (len(profile), len(hitches)))
        ok = False
    else:
        print('selftest ok: parsed 2 profile / 2 hitch lines')

    class Args(object):
        max_frontend_us = 1000.0
        max_draws = 60.0
        max_hitch_ms = 100.0

    if judge(stats, Args):
        print('selftest FAIL: thresholds rejected a healthy run')
        ok = False
    else:
        print('selftest ok: healthy run passes thresholds')

    Args.max_hitch_ms = 60.0
    if not judge(stats, Args):
        print('selftest FAIL: a 70.5ms hitch passed --max-hitch-ms 60')
        ok = False
    else:
        print('selftest ok: 70.5ms hitch rejected by --max-hitch-ms 60')

    print('profile_check selftest: %s' % ('PASSED' if ok else 'FAILED'))
    return 0 if ok else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', nargs='?', help='engine log with [cne-prof]/[cne-hitch] lines')
    parser.add_argument('--max-frontend-us', type=float, default=None)
    parser.add_argument('--max-draws', type=float, default=None)
    parser.add_argument('--max-hitch-ms', type=float, default=None)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if not args.log:
        parser.error('log path is required (or use --selftest)')

    profile, hitches = parse(args.log)
    stats = summarise(profile, hitches)
    problems = judge(stats, args)

    if args.json:
        print(json.dumps({'stats': stats, 'problems': problems}, sort_keys=True))
    else:
        print('[profile-check] %s: %s' % (args.log, json.dumps(stats, sort_keys=True)))
        for problem in problems:
            print('[profile-check] regression: %s' % problem)

    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
