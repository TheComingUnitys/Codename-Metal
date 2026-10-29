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
    entries, metallib = cache_counts()
    expected = json.loads(read(EXPECTED))
    wanted_entries = int(expected.get('cacheEntries', entries))
    wanted_min = int(expected.get('minimumMetallibStages', 0))

    problems = []
    if entries != wanted_entries:
        problems.append('cache has %d entries, expected %d (metal/audit/aot-expected.json)'
                        % (entries, wanted_entries))
    if metallib < wanted_min:
        problems.append('cache has %d .metallib stages, expected at least %d'
                        % (metallib, wanted_min))
    if metallib > entries:
        problems.append('metallib count %d > cache entries %d (corrupt cache?)' % (metallib, entries))

    readme = read(README)
    todo = read(TODO)

    readme_cov = expect_one(r'\*\*(\d+) cached shader stages\*\*', readme, 'README.MD', 'cached stage count')
    readme_libs = expect_one(r'\*\*(\d+) / 116\*\*', readme, 'README.MD', 'prebuilt library count')
    readme_missing = expect_one(r'- \*\*(\d+)\*\* shader stages have no prebuilt', readme, 'README.MD', 'missing stage count')
    todo_cov = expect_one(r'\*\*(\d+) / 116\*\*', todo, 'docs/METAL-TODO.md', 'coverage table cell')
    todo_failed = expect_one(r'Build-time GLSL to MSL failures \| \*\*(\d+) stages\*\*', todo,
                             'docs/METAL-TODO.md', 'failure table cell')
    todo_heading = expect_one(r'### 2\.1 The (\d+) stages that still fail', todo,
                              'docs/METAL-TODO.md', 'section 2.1 heading')

    for where, value, actual, label in [
            ('README.MD', readme_cov, entries, 'cache entries'),
            ('README.MD', readme_libs, metallib, 'metallib stages'),
            ('README.MD', readme_missing, len(expected.get('allowedMetallibFailures', {})), 'fallback stages'),
            ('docs/METAL-TODO.md', todo_cov, metallib, 'metallib stages'),
            ('docs/METAL-TODO.md', todo_failed, len(expected.get('allowedMetallibFailures', {})), 'fallback stages'),
            ('docs/METAL-TODO.md', todo_heading, len(expected.get('allowedMetallibFailures', {})), 'fallback stages'),
    ]:
        if value is not None and value != actual:
            problems.append('%s says %s = %d, cache says %d' % (where, label, value, actual))

    if problems:
        sys.stderr.write('error: AOT documentation is stale:\n')
        for problem in problems:
            sys.stderr.write('  - %s\n' % problem)
        sys.stderr.write('hint: run python3 metal/scripts/gen_shader_aot.py and update the docs\n')
        return 1

    print('[check-docs] cache %d entries, %d .metallib stages; README/TODO numbers match'
          % (entries, metallib))
    return 0


if __name__ == '__main__':
    sys.exit(main())
