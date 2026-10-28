#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_shader_aot.py -- build the AOT (ahead-of-time) GLSL -> MSL -> .metallib cache.

Codename Engine used to translate every GLSL shader to Metal Shading Language *while
the game ran* (`lime.graphics.metal.GLSLToMSL.translateSafe` called from the
`shaderSource` hook).  This script moves the whole shader pipeline to build time:

  1. enumerate every GLSL source that can reach the translator
     (`metal/scripts/aot_shaders.py` reproduces OpenFL's `ShaderMacro` expansion and
     Codename Engine's `FunkinShader` `#pragma` expansion byte for byte),
  2. translate all of them once with the *same* `GLSLToMSL` (`haxe --interp`,
     no C++ toolchain needed),
  3. compile every MSL stage to a real `.metallib` with `metal/tools/CompileShaderAot.mm`
     (the system Metal compiler, driven at build time; the game then loads the bytes
     with `newLibraryWithData` and never calls `newLibraryWithSource`), and
  4. bake `hash(source) -> (MSL, metallib)` (plus the attribute/uniform tables the
     runtime needs) into `metal/haxe/lime/graphics/metal/ShaderAotCache.hx`, which is
     compiled into the application.

At runtime `lime.graphics.metal.ShaderAot` only hashes and looks up; a miss is a loud
error and never falls back to runtime translation.  The `.metallib` is an *acceleration*
path: if one cannot be built (or this macOS cannot load it, e.g. after an OS update),
that stage falls back to the cached MSL and `newLibraryWithSource` -- the game still
renders, and the runtime audit reports the fallback.

`apply.sh` calls this automatically before every build.  Run it by hand with:

    python3 metal/scripts/gen_shader_aot.py [--project DIR] [--out FILE] [--check]
                                            [--skip-metallib] [--force-metallib]
"""

import argparse
import base64
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import aot_shaders  # noqa: E402

METAL_DIR = os.path.dirname(HERE)
DEFAULT_PROJECT = os.path.dirname(METAL_DIR)
DEFAULT_OUT = os.path.join(METAL_DIR, 'haxe', 'lime', 'graphics', 'metal', 'ShaderAotCache.hx')
REPORT_OUT = os.path.join(METAL_DIR, 'audit', 'aot-coverage.md')
TRANSLATOR_MAIN = os.path.join(METAL_DIR, 'tools', 'GenShaderAot.hx')

# Build-time MSL -> .metallib compiler (Objective-C++, needs only Command Line Tools).
METALLIB_TOOL_SRC = os.path.join(METAL_DIR, 'tools', 'CompileShaderAot.mm')
METALLIB_TOOL_BIN = os.path.join(METAL_DIR, 'tools', 'build', 'CompileShaderAot')
METALLIB_CACHE_ROOT = os.path.join(METAL_DIR, 'generated', 'metallibs')

# Bumped whenever the embedded payload format changes (invalidates nothing at runtime,
# but keeps the generated cache readable in the audit).
PAYLOAD_VERSION = 2


# --------------------------------------------------------------------- helpers

def haxe_string(text):
    """Escape `text` into a Haxe double-quoted string literal."""
    out = []
    for ch in text:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '$':
            out.append('\\$')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20:
            out.append('\\x%02x' % ord(ch))
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def chunked_haxe_string(text, size=8192):
    """Same as `haxe_string`, but split into `"a" + "b" + ...` lines.

    Splitting the *raw* text is safe because every chunk is escaped on its own; an
    escape sequence cut in half only produces `"\\" + "n..."`, which concatenates back
    to the original two characters.
    """
    parts = [haxe_string(text[i:i + size]) for i in range(0, len(text), size)]
    if not parts:
        return '""'
    return '\n\t\t+ '.join(parts)


def fingerprint(entries):
    payload = '\n'.join(e['k'] + ':' + e['n'] for e in entries)
    return '%08X%08X' % (aot_shaders.fnv1a(payload), aot_shaders.djb2(payload))


def relative(path, project):
    """Short, stable label for a path: project-relative, `~/...` for the lime dev checkout."""
    path = os.path.abspath(path)
    project = os.path.abspath(project)
    if path == project:
        return '.'
    if path.startswith(project + os.sep):
        return os.path.relpath(path, project)
    home = os.path.expanduser('~')
    if path.startswith(home + os.sep):
        return '~/' + os.path.relpath(path, home)
    return path


# ----------------------------------------------------------------- translation

def translate(entries, project, keep_temp=False):
    """Run the real GLSLToMSL over every entry through `haxe --interp`."""
    work = tempfile.mkdtemp(prefix='cne-aot-')
    manifest_path = os.path.join(work, 'manifest.json')
    out_path = os.path.join(work, 'out.json')

    manifest = [{'id': e['k'], 'stage': 'vertex' if e['s'] == 'vertex' else 'fragment', 'source': e['src']}
                for e in entries]
    with open(manifest_path, 'w', encoding='utf-8') as fh:
        json.dump(manifest, fh)

    cmd = [os.environ.get('HAXE', 'haxe'),
           '-cp', os.path.join(METAL_DIR, 'haxe'),
           '-cp', os.path.dirname(TRANSLATOR_MAIN),
           '--run', 'GenShaderAot', manifest_path, out_path]
    sys.stdout.write('[gen-shader-aot] translating %d shader stage(s) with %s\n' % (len(entries), ' '.join(cmd[:1])))
    sys.stdout.flush()
    # The helper exits 3 when a translation fails; the failures are reported below with
    # their source identifiers, so do not turn the exit code into an exception here.
    subprocess.call(cmd, cwd=project)
    if not os.path.exists(out_path):
        sys.stderr.write('error: the build-time translator produced no output (%s)\n' % out_path)
        return entries

    with open(out_path, 'r', encoding='utf-8') as fh:
        results = json.load(fh)

    for entry, result in zip(entries, results):
        entry['ok'] = bool(result.get('ok'))
        entry['errors'] = result.get('errors') or []
        entry['msl'] = result.get('code')
        entry['result'] = result

    if keep_temp:
        sys.stdout.write('[gen-shader-aot] temp dir kept at %s\n' % work)
    return entries


# ------------------------------------------------------------------- metallib
#
# MSL -> .metallib at build time.  This machine has only Command Line Tools, so the
# Xcode `metal` compiler does not exist; `metal/tools/CompileShaderAot.mm` uses the
# Metal compiler that *is* on the system (the one `newLibraryWithSource` drives) inside
# a throw-away build-time process, serializes the MTLLibrary with a private selector,
# and re-loads the bytes with the public `newLibraryWithData` API to verify them.
#
# Everything here is best-effort by design:
#   * tool cannot be built / private selector missing -> MSL payload only (the game
#     keeps working, it just compiles the MSL at startup like before),
#   * one stage fails                                  -> that stage gets MSL only,
#   * OS/GPU changed since the cache was written       -> the tool recompiles, and the
#     runtime additionally re-validates every metallib before trusting it.
#
# The cache is keyed by "<macOS build>-<arch>" and additionally stores a per-entry meta
# file (MSL fingerprint + device tag + size), so an OS update invalidates it instead of
# silently reusing a `.metallib` whose embedded toolkit version no longer matches.

def metallib_environment_tag():
    """Filesystem-safe identifier of *this* build machine / OS build."""
    mac = platform.mac_ver()[0] or 'macos-unknown'
    try:
        build = subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip()
    except Exception:
        build = 'build-unknown'
    arch = platform.machine() or 'arch-unknown'
    raw = '%s_%s_%s' % (mac, build, arch)
    return ''.join(ch if (ch.isalnum() or ch in '._-') else '_' for ch in raw)


def ensure_metallib_tool(quiet=False):
    """Build `metal/tools/CompileShaderAot.mm` when missing or older than its source."""
    if not os.path.isfile(METALLIB_TOOL_SRC):
        return None, 'missing %s' % METALLIB_TOOL_SRC
    try:
        if (os.path.isfile(METALLIB_TOOL_BIN)
                and os.path.getmtime(METALLIB_TOOL_BIN) >= os.path.getmtime(METALLIB_TOOL_SRC)):
            return METALLIB_TOOL_BIN, None
        os.makedirs(os.path.dirname(METALLIB_TOOL_BIN), exist_ok=True)
        compiler = os.environ.get('CLANGXX') or shutil.which('xcrun') and 'xcrun' or 'clang++'
        if compiler == 'xcrun':
            cmd = ['xcrun', 'clang++']
        else:
            cmd = [compiler]
        cmd += ['-x', 'objective-c++', '-fobjc-arc', '-std=c++17', '-O2',
                METALLIB_TOOL_SRC, '-framework', 'Metal', '-framework', 'Foundation',
                '-o', METALLIB_TOOL_BIN]
        if not quiet:
            sys.stdout.write('[gen-shader-aot] building %s (clang++ + Metal.framework)\n'
                             % relative(METALLIB_TOOL_BIN, DEFAULT_PROJECT))
            sys.stdout.flush()
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 0:
            return None, 'clang++ failed (%d):\n%s' % (result.returncode, result.stdout[-4000:])
        return METALLIB_TOOL_BIN, None
    except Exception as exc:  # noqa: BLE001 - a missing tool must never fail the build
        return None, 'could not build the metallib tool: %s' % exc


def compile_metallibs(entries, force=False, quiet=False):
    """Compile every entry's MSL to a `.metallib` and return {key: base64} + stats.

    Never raises: metallib generation is an acceleration path, so any failure leaves
    the entries on the MSL-only payload and is reported loudly in the build log.
    """
    stats = {
        'available': False,
        'ok': 0, 'fail': 0, 'cached': 0, 'bytes': 0,
        'compile_ms': 0.0, 'failures': [], 'reason': None,
        'device': None,
    }

    tool, error = ensure_metallib_tool(quiet=quiet)
    if tool is None:
        stats['reason'] = error
        sys.stdout.write('[gen-shader-aot] WARNING: .metallib generation disabled - %s\n' % error)
        return {}, stats, {}

    manifest = [{'k': e['k'], 'stage': e['s'],
                 'msl': '// [cne-aot] key=' + e['k'] + '\n' + e['result']['code']}
                for e in entries]

    work = tempfile.mkdtemp(prefix='cne-mtlaot-')
    manifest_path = os.path.join(work, 'manifest.json')
    with open(manifest_path, 'w', encoding='utf-8') as fh:
        json.dump(manifest, fh)

    tag = metallib_environment_tag()
    cache_dir = os.path.join(METALLIB_CACHE_ROOT, tag)
    cmd = [tool, manifest_path, cache_dir]
    if force:
        cmd.append('--force')
    if quiet:
        cmd.append('--quiet')

    if not quiet:
        sys.stdout.write('[gen-shader-aot] compiling %d MSL stage(s) to .metallib '
                         '(cache: %s)\n' % (len(entries), relative(cache_dir, DEFAULT_PROJECT)))
        sys.stdout.flush()
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except Exception as exc:  # noqa: BLE001
        stats['reason'] = 'could not run the metallib tool: %s' % exc
        sys.stdout.write('[gen-shader-aot] WARNING: .metallib generation disabled - %s\n' % stats['reason'])
        return {}, stats, {}

    if not quiet and result.stdout:
        # The tool's per-entry log is useful; surface it verbatim (it is short).
        sys.stdout.write(result.stdout)
        sys.stdout.flush()

    if result.returncode == 4:
        stats['reason'] = ('the private `_MTLLibrary -serializeToURL:error:` API is not '
                           'available on this macOS; falling back to runtime source compile')
        sys.stdout.write('[gen-shader-aot] WARNING: %s\n' % stats['reason'])
        return {}, stats, {}

    summary_path = os.path.join(cache_dir, 'summary.json')
    if result.returncode != 0 or not os.path.isfile(summary_path):
        stats['reason'] = 'metallib tool failed (exit %d)' % result.returncode
        sys.stdout.write('[gen-shader-aot] WARNING: %s\n' % stats['reason'])
        return {}, stats, {}

    with open(summary_path, 'r', encoding='utf-8') as fh:
        summary = json.load(fh)

    stats.update({
        'available': True,
        'ok': int(summary.get('ok', 0)),
        'fail': int(summary.get('fail', 0)),
        'cached': int(summary.get('cached', 0)),
        'bytes': int(summary.get('bytes', 0)),
        'compile_ms': float(summary.get('compileMs', 0.0)),
        'device': summary.get('device'),
    })

    by_key = {}
    sizes = {}
    for item in summary.get('entries', []):
        if not item.get('ok'):
            stats['failures'].append((item.get('k'), item.get('stage'), item.get('error', 'unknown')))
            continue
        path = os.path.join(cache_dir, item['k'] + '.metallib')
        try:
            with open(path, 'rb') as fh:
                raw = fh.read()
            sizes[item['k']] = len(raw)
            by_key[item['k']] = base64.b64encode(raw).decode('ascii')
        except OSError as exc:
            stats['failures'].append((item.get('k'), item.get('stage'), 'unreadable: %s' % exc))

    return by_key, stats, sizes


# --------------------------------------------------------------------- output

def build_payload(entries):
    payload = []
    for entry in sorted(entries, key=lambda e: e['k']):
        result = entry['result']
        item = {
            'k': entry['k'],
            'n': entry['n'],
            's': entry['s'],
            'm': '// [cne-aot] key=' + entry['k'] + '\n' + result['code'],
            'z': result['uniformSize'],
            'a': result['attributes'],
            'u': result['uniforms'],
            't': result['samplers'],
        }
        # `b` = base64 .metallib (built by tools/CompileShaderAot.mm).  Absent when the
        # stage could not be compiled ahead of time; the runtime then compiles the
        # cached MSL source instead (counted by the shader audit).
        if entry.get('metallib'):
            item['b'] = entry['metallib']
        payload.append(item)
    return payload


CACHE_HEADER = '''// -----------------------------------------------------------------------------
// GENERATED FILE - DO NOT EDIT.
//
// AOT (ahead-of-time) GLSL -> Metal Shading Language cache for the bgfx/Metal
// backend.  Regenerate with:
//
//     python3 metal/scripts/gen_shader_aot.py
//
// (metal/scripts/apply.sh does this automatically before every build.)
//
// The runtime counterpart is `lime.graphics.metal.ShaderAot`, which hashes the GLSL
// that OpenFL assembled and looks it up here.  A hit is used directly - the
// GLSL->MSL translator is never invoked while the game runs.  See
// metal/audit/aot-shaders.md for the coverage table and the enumeration method.
// -----------------------------------------------------------------------------
package lime.graphics.metal;

class ShaderAotCache
{
	/** Number of cached shader stages. */
	public static inline var COUNT:Int = %(count)d;

	/** Content fingerprint (FNV-1a32+DJB2-32 over every cache key). */
	public static inline var FINGERPRINT:String = "%(fingerprint)s";

	/** Stage kinds present in the cache, for the startup/exit report. */
	public static inline var STAGES:String = "%(stages)s";

	/**
		JSON payload: array of
		`{k: exact key, n: normalised key, s: "vertex"|"fragment", m: MSL,
		  b: base64 .metallib (optional), z: uniformSize, a: attributes,
		  u: uniforms, t: samplers}`.
		Parsed once, lazily, by `ShaderAot.ensureLoaded()`.  `b` is the build-time
		compiled Metal library; when present the runtime hands the bytes to bgfx and
		never calls `newLibraryWithSource`.  `m` stays as the fallback for the case
		where this macOS/GPU cannot load that .metallib.
	**/
	public static var DATA(default, null):String =
		%(data)s;
}
'''


def write_cache(entries, out_path):
    payload = build_payload(entries)
    text = json.dumps(payload, separators=(',', ':'))
    stages = sorted(set(e['s'] for e in entries))
    content = CACHE_HEADER % {
        'count': len(entries),
        'fingerprint': fingerprint(payload),
        'stages': ','.join(stages),
        'data': chunked_haxe_string(text),
    }
    changed = True
    if os.path.exists(out_path):
        with open(out_path, 'r', encoding='utf-8') as fh:
            changed = fh.read() != content
    if changed:
        with open(out_path, 'w', encoding='utf-8') as fh:
            fh.write(content)
    return len(content), changed


def write_report(entries, out_path, project, roots, metallib_stats=None):
    """Auto-generated coverage table (referenced by metal/audit/aot-shaders.md)."""
    lines = []
    lines.append('# AOT shader cache - generated coverage table')
    lines.append('')
    lines.append('> Generated by `metal/scripts/gen_shader_aot.py`. Do not edit by hand.')
    lines.append('')
    lines.append('Sources enumerated from:')
    lines.append('')
    for root in roots:
        lines.append('* `%s`' % relative(root, project))
    lines.append('')
    lines.append('## Cache summary')
    lines.append('')
    lines.append('| metric | value |')
    lines.append('| --- | --- |')
    metallib_entries = [e for e in entries if e.get('metallib')]
    metallib_bytes = sum(e.get('metallib_bytes', 0) for e in metallib_entries)
    lines.append('| cached shader stages | %d |' % len(entries))
    lines.append('| unique source keys | %d |' % len(set(e['k'] for e in entries)))
    lines.append('| vertex / fragment | %d / %d |'
                 % (sum(1 for e in entries if e['s'] == 'vertex'), sum(1 for e in entries if e['s'] == 'fragment')))
    lines.append('| cache fingerprint | `%s` |' % fingerprint(build_payload(entries)))
    lines.append('| total MSL bytes | %d |' % sum(len(e['result']['code']) for e in entries))
    lines.append('| **AOT .metallib stages** | **%d / %d** |' % (len(metallib_entries), len(entries)))
    lines.append('| **AOT .metallib bytes** | **%d** |' % metallib_bytes)
    if metallib_stats and metallib_stats.get('device'):
        lines.append('| .metallib build environment | `%s` |' % metallib_stats.get('device'))
    if metallib_stats and not metallib_stats.get('available'):
        lines.append('| .metallib generation | disabled: %s |' % (metallib_stats.get('reason') or 'unknown'))
    lines.append('')
    lines.append('## Every entry')
    lines.append('')
    lines.append('| # | source | stage | cache key (exact) | normalised key | MSL bytes | .metallib bytes | uniforms | attributes | samplers |')
    lines.append('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    for index, entry in enumerate(sorted(entries, key=lambda e: (e['kind'], e['ident']))):
        result = entry['result']
        lib = str(entry.get('metallib_bytes')) if entry.get('metallib') else '-'
        lines.append('| %d | `%s` | %s | `%s` | `%s` | %d | %s | %d | %d | %d |' % (
            index + 1, entry['ident'], entry['s'], entry['k'], entry['n'],
            len(result['code']), lib, len(result['uniforms']), len(result['attributes']), len(result['samplers'])))
    lines.append('')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')
    return out_path


# ----------------------------------------------------------------------- main

def collect(project):
    sources, classes, roots = aot_shaders.build_sources(project)
    entries = []
    seen = set()
    for source in sources:
        if source.key in seen:
            continue
        seen.add(source.key)
        entries.append({
            'ident': source.ident,
            'kind': source.kind,
            's': source.stage,
            'src': source.source,
            'k': source.key,
            'n': source.norm,
        })
    return entries, classes, roots


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--project', default=DEFAULT_PROJECT, help='Codename Engine project root')
    parser.add_argument('--out', default=DEFAULT_OUT, help='ShaderAotCache.hx output path')
    parser.add_argument('--report', default=REPORT_OUT, help='generated coverage table output path')
    parser.add_argument('--check', action='store_true', help='fail if the checked-in cache is stale')
    parser.add_argument('--keep-temp', action='store_true', help='keep the haxe manifest/result files')
    parser.add_argument('--skip-metallib', action='store_true',
                        help='do not compile .metallib payloads (MSL is compiled at runtime instead)')
    parser.add_argument('--force-metallib', action='store_true',
                        help='recompile every .metallib even if the incremental cache is valid')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args(argv)

    project = os.path.abspath(os.path.expanduser(args.project))
    if not os.path.isdir(os.path.join(project, 'assets')):
        sys.stderr.write('error: %s does not look like the Codename Engine project\n' % project)
        return 2
    if not os.path.exists(TRANSLATOR_MAIN):
        sys.stderr.write('error: missing build-time translator main: %s\n' % TRANSLATOR_MAIN)
        return 2

    entries, classes, roots = collect(project)
    if not entries:
        sys.stderr.write('error: no GLSL sources found - refusing to write an empty cache\n')
        return 2

    translate(entries, project, keep_temp=args.keep_temp)

    failed = [e for e in entries if not e['ok']]
    if failed:
        sys.stderr.write('error: %d shader stage(s) failed to translate:\n' % len(failed))
        for entry in failed:
            sys.stderr.write('  %-60s %s\n' % (entry['ident'], '; '.join(entry['errors'])))
        return 3

    # ---- step 2b: MSL -> .metallib (build-time compiled Metal libraries) ----------
    # This is an acceleration path, never a build blocker: if anything goes wrong the
    # cache just carries MSL and the runtime compiles it with newLibraryWithSource
    # (which the shader audit then reports explicitly).
    metallib_stats = {'available': False, 'reason': 'skipped (--skip-metallib)'}
    metallib_by_key = {}
    if args.skip_metallib:
        sys.stdout.write('[gen-shader-aot] .metallib generation skipped (--skip-metallib)\n')
    else:
        metallib_by_key, metallib_stats, metallib_sizes = compile_metallibs(
            entries, force=args.force_metallib, quiet=args.quiet)
        for entry in entries:
            entry['metallib'] = metallib_by_key.get(entry['k'])
            entry['metallib_bytes'] = metallib_sizes.get(entry['k'], 0)
        if metallib_stats.get('available') and not args.quiet:
            print('[gen-shader-aot] .metallib compiled         : %d / %d stages (%d bytes, %d cached, '
                  'compile %.1fms)'
                  % (metallib_stats['ok'], len(entries), metallib_stats['bytes'],
                     metallib_stats['cached'], metallib_stats['compile_ms']))
            if metallib_stats['failures']:
                print('[gen-shader-aot] .metallib fallback stages  : %d (runtime will compile their MSL)'
                      % len(metallib_stats['failures']))
                for key, stage, error in metallib_stats['failures']:
                    print('[gen-shader-aot]   %s %-8s %s' % (key, stage, error))

    size, changed = write_cache(entries, args.out)
    if not args.check:
        write_report(entries, args.report, project, roots, metallib_stats=metallib_stats)

    stages = {}
    for entry in entries:
        stages[entry['s']] = stages.get(entry['s'], 0) + 1
    if not args.quiet:
        print('[gen-shader-aot] classes with @:gl metadata : %d' % len(classes))
        print('[gen-shader-aot] cache entries               : %d (%d vertex, %d fragment)'
              % (len(entries), stages.get('vertex', 0), stages.get('fragment', 0)))
        kinds = {}
        for entry in entries:
            kinds[entry['kind']] = kinds.get(entry['kind'], 0) + 1
        print('[gen-shader-aot] by source kind              : %s'
              % ', '.join('%s=%d' % (k, kinds[k]) for k in sorted(kinds)))
        print('[gen-shader-aot] fingerprint                 : %s' % fingerprint(build_payload(entries)))
        print('[gen-shader-aot] %s %s (%d bytes)'
              % ('wrote' if changed else 'unchanged', relative(args.out, project), size))
        if not args.check:
            print('[gen-shader-aot] wrote %s' % relative(args.report, project))

    if args.check:
        # regenerate into memory and compare against the checked-in file
        expected = os.path.join(tempfile.mkdtemp(prefix='cne-aot-check-'), 'ShaderAotCache.hx')
        write_cache(entries, expected)
        if not os.path.exists(args.out) or open(args.out, 'rb').read() != open(expected, 'rb').read():
            sys.stderr.write('error: %s is stale - run python3 metal/scripts/gen_shader_aot.py\n'
                             % relative(args.out, project))
            return 4
        print('[gen-shader-aot] cache is up to date')

    return 0


if __name__ == '__main__':
    sys.exit(main())
