#!/usr/bin/env python3
"""Checks that the AOT numbers quoted in README.MD / docs/METAL-TODO.md match the cache.

The cache itself is generated (ShaderAotCache.hx), so every number in the docs is a
snapshot that silently goes stale after a shader change.  Run this in CI / pre-push.

Exit codes: 0 consistent, 1 mismatch (with a list), 2 unreadable input.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
METAL_DIR = os.path.dirname(HERE)
PROJECT = os.path.dirname(METAL_DIR)          # CodenameEngine-1.0.1
REPO = os.path.dirname(PROJECT)              # repository root
CACHE = os.path.join(METAL_DIR, 'haxe', 'lime', 'graphics', 'metal', 'ShaderAotCache.hx')
EXPECTED = os.path.join(METAL_DIR, 'audit', 'aot-expected.json')
README = os.path.join(REPO, 'README.MD')
TODO = os.path.join(REPO, 'docs', 'METAL-TODO.md')


def read(path):
    try:
        with open(path, 'r', encoding='utf-8') as fh:
            return fh.read()
    except OSError as exc:
        sys.stderr.write('error: cannot read %s: %s\n' % (path, exc))
        sys.exit(2)


def cache_counts():
    text = read(CACHE)
    count = re.search(r'COUNT\s*:\s*Int\s*=\s*(\d+)', text)
    if not count:
        sys.stderr.write('error: COUNT:Int not found in %s\n' % CACHE)
        sys.exit(2)
    payload = text.split('DATA(default, null):String =', 1)
    if len(payload) != 2:
        sys.stderr.write('error: DATA string not found in %s\n' % CACHE)
        sys.exit(2)
    # The payload is one JSON document split into Haxe string chunks.  Counting the
    # metallib marker is enough for the audit (the generator writes "b": for them).
    metallib = len(re.findall(r'\\"b\\"\s*:\s*\\"', payload[1]))
    return int(count.group(1)), metallib


def expect_one(pattern, text, where, label):
    match = re.search(pattern, text)
    if not match:
        sys.stderr.write('warning: %s: cannot find %s (pattern %r)\n' % (where, label, pattern))
        return None
    return int(match.group(1))


def main():
    expected = json.loads(read(EXPECTED))
    wanted_entries = int(expected.get('cacheEntries', 0))
    wanted_min = int(expected.get('minimumMetallibStages', 0))

    problems = []
    if os.path.exists(CACHE):
        entries, metallib = cache_counts()
        if entries < wanted_entries:
            problems.append('cache has %d entries, expected at least the %d-stage baseline'
                            % (entries, wanted_entries))
        if metallib < wanted_min:
            problems.append('cache has %d .metallib stages, expected at least %d'
                            % (metallib, wanted_min))
        if metallib > entries:
            problems.append('metallib count %d > cache entries %d (corrupt cache?)'
                            % (metallib, entries))
    else:
        # The cache is generated build output now (see .gitignore and gen_shader_aot.py),
        # so a clean checkout has no ShaderAotCache.hx.  The documented numbers refer to
        # the committed baseline in metal/audit/aot-expected.json.
        entries = wanted_entries
        metallib = wanted_min
        print('[check-docs] cache not generated yet; checking against the committed baseline')

    # The README/TODO numbers describe the baseline corpus.  Runtime-composed shader
    # sources appended by --include-misses may make the generated cache larger; the docs
    # keep quoting the baseline so they do not depend on the local mod set.
    baseline_missing = max(0, wanted_entries - wanted_min)

    readme = read(README)
    todo = read(TODO)

    readme_cov = expect_one(r'\*\*(\d+) cached shader stages\*\*', readme, 'README.MD', 'cached stage count')
    ratio = r'\*\*(\d+) / ' + str(wanted_entries) + r'\*\*'
    readme_libs = expect_one(ratio, readme, 'README.MD', 'prebuilt library count')
    todo_cov = expect_one(ratio, todo, 'docs/METAL-TODO.md', 'coverage table cell')

    for where, value, actual, label in [
            ('README.MD', readme_cov, wanted_entries, 'cache entries'),
            ('README.MD', readme_libs, wanted_min, 'metallib stages'),
            ('docs/METAL-TODO.md', todo_cov, wanted_min, 'metallib stages'),
    ]:
        if value is not None and value != actual:
            problems.append('%s says %s = %d, cache says %d' % (where, label, value, actual))

    if problems:
        sys.stderr.write('error: AOT documentation is stale:\n')
        for problem in problems:
            sys.stderr.write('  - %s\n' % problem)
        sys.stderr.write('hint: run python3 metal/scripts/gen_shader_aot.py and update the docs\n')
        return 1

    print('[check-docs] %d-entry / %d-metallib baseline; README/TODO numbers match'
          % (wanted_entries, wanted_min))
    return 0


if __name__ == '__main__':
    sys.exit(main())
