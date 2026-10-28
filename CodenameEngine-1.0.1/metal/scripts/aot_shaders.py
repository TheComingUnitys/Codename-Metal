#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Extract every GLSL source that can reach the runtime GLSL->MSL translator.

This is the enumeration half of the build-time AOT shader pipeline
(`metal/scripts/gen_shader_aot.py` drives it).  It reads the actual Haxe sources
and reproduces, byte for byte, what OpenFL's `ShaderMacro` build macro and
Codename Engine's `FunkinShader` source setters hand to `glShaderSource`.
"""
import os
import re
import sys

ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'", '$': '$'}


def parse_haxe_string(src, i):
    """Parse a Haxe string literal starting at src[i] (the quote).  Returns (value, next_index)."""
    quote = src[i]
    i += 1
    out = []
    n = len(src)
    while i < n:
        c = src[i]
        if c == '\\':
            i += 1
            if i >= n:
                break
            e = src[i]
            if e in ESCAPES:
                out.append(ESCAPES[e])
                i += 1
            elif e == 'x':
                out.append(chr(int(src[i + 1:i + 3], 16)))
                i += 3
            elif e == 'u':
                out.append(chr(int(src[i + 1:i + 5], 16)))
                i += 5
            else:
                out.append(e)
                i += 1
            continue
        if c == quote:
            return ''.join(out), i + 1
        out.append(c)
        i += 1
    raise ValueError('unterminated string literal at %d' % i)


META_RE = re.compile(r'@:gl(Vertex|Fragment)(Source|Header|Body)\s*\(\s*(["\'])')
CLASS_RE = re.compile(
    r'(?:^|\n)[ \t]*(?:(?:private|final|extern)\s+)*class\s+([A-Za-z_]\w*)'
    r'(?:\s*<[^>\n]*>)?(?:\s+extends\s+([A-Za-z_][\w.]*))?')
PKG_RE = re.compile(r'^[ \t]*package\s+([\w.]+)\s*;?', re.MULTILINE)
TYPEDEF_RE = re.compile(r'(?:^|\n)[ \t]*(?:(?:private|public)\s+)?typedef\s+([A-Za-z_]\w*)\s*=\s*([^;]+);')
TYPE_TOKEN_RE = re.compile(r'[A-Za-z_][\w.]*')
NON_CLASS_TOKENS = {'Dynamic', 'nme', 'if', 'else', 'end', 'Null', 'Void', 'Int', 'Float', 'Bool', 'String'}


def strip_comments_for_class_scan(src):
    """Blank out comments, keeping every byte offset intact.

    The caller compares positions in this text with positions in the original source
    (to attribute each `@:gl*` annotation to the class that precedes it), so comments
    must be replaced by whitespace of the same length - not removed.
    """
    out = list(src)
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            if j < 0:
                j = n
            for k in range(i, j):
                out[k] = ' '
            i = j
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
            continue
        if c in '"\'':
            try:
                _, nxt = parse_haxe_string(src, i)
            except ValueError:
                i += 1
                continue
            i = nxt
            continue
        i += 1
    return ''.join(out)


class ClassDef(object):
    def __init__(self, name, package, super_name, path, order):
        self.name = name
        self.package = package
        self.full = (package + '.' + name) if package else name
        self.super_name = super_name
        self.path = path
        self.order = order
        self.meta = {}  # kind -> list of strings (source order)


def scan_file(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        src = fh.read()
    if '@:gl' not in src:
        return []
    m = PKG_RE.search(src[:8000])
    package = m.group(1) if m else ''
    stripped = strip_comments_for_class_scan(src)
    classes = []
    for m in CLASS_RE.finditer(stripped):
        classes.append(ClassDef(m.group(1), package, m.group(2), path, m.start()))
    if not classes:
        return []
    for m in META_RE.finditer(src):
        kind = 'gl' + m.group(1) + m.group(2)
        value, _ = parse_haxe_string(src, m.start(3))
        owner = None
        for cls in classes:
            if cls.order < m.start():
                owner = cls
        if owner is None:
            owner = classes[0]
        owner.meta.setdefault(kind, []).append(value)
    return classes


def scan_typedefs(path):
    """`typedef FlxShader = flixel.graphics.tile.FlxGraphicsShader;` aliases share the
    superclass chain of their target, so they have to be resolved like real classes."""
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        src = fh.read()
    if 'typedef' not in src:
        return {}
    out = {}
    for match in TYPEDEF_RE.finditer(src):
        name = match.group(1)
        rhs = match.group(2)
        candidates = [tok for tok in TYPE_TOKEN_RE.findall(rhs) if tok not in NON_CLASS_TOKENS]
        # last candidate wins (`#if nme Dynamic #else <real> #end`)
        if candidates:
            out[name] = candidates
    return out


def collect_types(roots):
    """Returns (class definitions, typedef name -> candidate target type names)."""
    classes = []
    typedefs = {}
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in ('node_modules', '.git')]
            for fn in filenames:
                if not fn.endswith('.hx'):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    classes.extend(scan_file(path))
                    typedefs.update(scan_typedefs(path))
                except Exception as exc:  # noqa: BLE001
                    sys.stderr.write('warn: %s: %s\n' % (path, exc))
    return classes, typedefs


def collect_classes(roots):
    return collect_types(roots)[0]


_TYPEDEFS = {}


def register_typedefs(typedefs):
    _TYPEDEFS.clear()
    _TYPEDEFS.update(typedefs or {})


def make_index(classes, typedefs=None):
    register_typedefs(typedefs)
    by_full = {}
    by_name = {}
    for cls in classes:
        by_full.setdefault(cls.full, []).append(cls)
        by_name.setdefault(cls.name, []).append(cls)
    return by_full, by_name


def resolve_super(cls, by_full, by_name):
    if not cls.super_name:
        return None
    if cls.super_name in by_full:
        return by_full[cls.super_name][0]
    short = cls.super_name.split('.')[-1]
    if short in by_name:
        return by_name[short][0]
    return None


def lookup_type(name, by_full, by_name, depth=0):
    """Resolve a type name (class, or typedef alias of a class) to candidate classes."""
    if depth > 8:
        return []
    if name in by_full:
        return list(by_full[name])
    short = name.split('.')[-1]
    if short in by_name:
        return list(by_name[short])
    if short in _TYPEDEFS:
        found = []
        for candidate in _TYPEDEFS[short]:
            found.extend(lookup_type(candidate, by_full, by_name, depth + 1))
        return found
    return []


def resolve_supers(cls, by_full, by_name):
    """All candidate definitions of a class' direct superclass."""
    if not cls.super_name:
        return [None]
    found = lookup_type(cls.super_name, by_full, by_name)
    return found or [None]


def ancestor_chain(cls, by_full, by_name):
    chain = []
    seen = set()
    cur = cls
    while cur is not None and cur.full not in seen:
        seen.add(cur.full)
        chain.append(cur)
        cur = resolve_super(cur, by_full, by_name)
    return chain


def ancestor_chain_variants(cls, by_full, by_name, cap=8):
    """Same as `ancestor_chain`, but branches when a superclass has several definitions.

    Several classes are declared twice in this tree on purpose (Lime/OpenFL keep a
    reduced copy, Codename Engine ships a formatting-only override, e.g.
    `openfl.display.GraphicsShader`).  The compiler only ever uses one of them, so
    the AOT cache gets an entry for every variant - extra entries are harmless.
    """
    chains = []

    def walk(chain, seen):
        if len(chains) >= cap:
            return
        cur = chain[-1]
        if not cur.super_name:
            chains.append(chain)
            return
        candidates = resolve_supers(cur, by_full, by_name)
        live = [c for c in candidates if c is not None and c.full not in seen]
        if not live:
            chains.append(chain)
            return
        for nxt in live:
            walk(chain + [nxt], seen | {nxt.full})

    walk([cls], {cls.full})
    return chains


def _expand_chain(chain):
    cur_sources = {}
    for kind in ('glVertexSource', 'glFragmentSource'):
        values = chain[0].meta.get(kind, [])
        cur_sources[kind] = values[-1] if values else None
    cur_headers = {}
    for kind in ('glVertexHeader', 'glFragmentHeader'):
        values = chain[0].meta.get(kind, [])
        cur_headers[kind] = values[-1] if values else ''
    cur_bodies = {}
    for kind in ('glVertexBody', 'glFragmentBody'):
        values = chain[0].meta.get(kind, [])
        cur_bodies[kind] = values[-1] if values else ''

    for kind in ('glVertexHeader', 'glFragmentHeader'):
        acc = cur_headers[kind]
        for link in chain[1:]:
            for value in link.meta.get(kind, []):
                acc = value + '\n' + acc
        cur_headers[kind] = acc
    for kind in ('glVertexBody', 'glFragmentBody'):
        acc = cur_bodies[kind]
        for link in chain[1:]:
            for value in link.meta.get(kind, []):
                acc = value + '\n' + acc
        cur_bodies[kind] = acc
    for kind in ('glVertexSource', 'glFragmentSource'):
        if cur_sources[kind] is None:
            for link in chain[1:]:
                values = link.meta.get(kind, [])
                if values:
                    cur_sources[kind] = values[0]
                    break

    vh = cur_headers['glVertexHeader']
    fh = cur_headers['glFragmentHeader']
    vb = cur_bodies['glVertexBody']
    fb = cur_bodies['glFragmentBody']

    # a class may declare its source twice under `#if/#else` (e.g. `#if emscripten`);
    # every variant gets its own cache entry.
    vertex_variants = list(chain[0].meta.get('glVertexSource', []))
    fragment_variants = list(chain[0].meta.get('glFragmentSource', []))
    if not vertex_variants and cur_sources['glVertexSource'] is not None:
        vertex_variants = [cur_sources['glVertexSource']]
    if not fragment_variants and cur_sources['glFragmentSource'] is not None:
        fragment_variants = [cur_sources['glFragmentSource']]

    combos = []
    for vs in (vertex_variants or [None]):
        for fs in (fragment_variants or [None]):
            combos.append({
                'vertex': None if vs is None else vs.replace('#pragma header', vh).replace('#pragma body', vb),
                'fragment': None if fs is None else fs.replace('#pragma header', fh).replace('#pragma body', fb),
            })
    return combos


def expand_class(cls, by_full, by_name):
    """Reproduce openfl.utils._internal.ShaderMacro.build() for one class.

    Returns a list of dicts {'vertex', 'fragment'} - already `#pragma`-expanded,
    but *without* the `#version`/precision prefix that `Shader.__initGL` adds.
    """
    combos = []
    seen = set()
    for chain in ancestor_chain_variants(cls, by_full, by_name):
        for combo in _expand_chain(chain):
            sig = (combo['vertex'], combo['fragment'])
            if sig in seen:
                continue
            seen.add(sig)
            combos.append(combo)
    return combos


# ---------------------------------------------------------------- prefixes

def openfl_prefix():
    """`openfl.display.Shader.__initGL` prefix (precisionHint == FULL)."""
    return ('#ifdef GL_ES\n'
            '#ifdef GL_FRAGMENT_PRECISION_HIGH\n'
            'precision highp float;\n'
            '#else\n'
            'precision mediump float;\n'
            '#endif\n'
            '#endif\n\n')


def funkin_prefix(glsl_ver='120', shader_prefix=''):
    """`funkin.backend.shaders.FunkinShader.__initGL` prefix."""
    return ('#version %s\n' % glsl_ver) + shader_prefix + (
        '#ifdef GL_ES\n'
        '#ifdef GL_FRAGMENT_PRECISION_HIGH\n'
        'precision highp float;\n'
        '#else\n'
        'precision mediump float;\n'
        '#endif\n'
        '#endif\n')


def is_funkin_shader(cls, by_full, by_name):
    return any(link.name == 'FunkinShader' for link in ancestor_chain(cls, by_full, by_name))


# ------------------------------------------------- Codename Engine shaders
#
# `funkin.backend.shaders.CustomShader` does not use `@:gl*` metadata: it reads
# `assets/shaders/<name>.frag` / `.vert` at runtime and `FunkinShader` expands the
# `#pragma header` / `#pragma body` placeholders with the `ShaderTemplates`
# constants.  Those constants are Haxe `static final` string literals in
# `FunkinShader.hx`; parse them from the source so there is a single source of
# truth (the runtime consistency check in `ShaderAot` compares them again).

TEMPLATE_RE = re.compile(r'public\s+static\s+(?:final|inline\s+var)\s+(\w+)\s*:\s*String\s*=\s*(["\'])')


def parse_shader_templates(funkin_shader_path):
    with open(funkin_shader_path, 'r', encoding='utf-8') as fh:
        src = fh.read()
    out = {}
    for m in TEMPLATE_RE.finditer(src):
        value, _ = parse_haxe_string(src, m.start(2))
        out[m.group(1)] = value
    return out


IMPORT_RE = re.compile(r'#import\s+<(.*)>')


def expand_imports(value, assets_dir):
    while True:
        m = IMPORT_RE.search(value)
        if not m:
            return value
        path = os.path.join(assets_dir, 'shaders', m.group(1))
        if not os.path.exists(path):
            return value
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            value = value.replace(m.group(0), fh.read())


def expand_funkin_fragment(value, templates, assets_dir):
    value = expand_imports(value, assets_dir)
    return value.replace('#pragma header', templates['fragHeader']).replace('#pragma body', templates['fragBody'])


def expand_funkin_vertex(value, templates, assets_dir):
    value = expand_imports(value, assets_dir)
    use_back_compat = True
    for pattern in (r'attribute float alpha', r'attribute vec4 colorMultiplier',
                    r'attribute vec4 colorOffset', r'uniform bool hasColorTransform'):
        if not re.search(pattern, value):
            use_back_compat = False
            break
    header = templates['vertHeaderBackCompat'] if use_back_compat else templates['vertHeader']
    body = templates['vertBodyBackCompat'] if use_back_compat else templates['vertBody']
    return value.replace('#pragma header', header).replace('#pragma body', body)


def funkin_assembled(frag_raw, vert_raw, templates, assets_dir, glsl_ver='120', shader_prefix='',
                     frag_prefix='', vert_prefix=''):
    """What `FunkinShader.__initGL` hands to `glShaderSource`."""
    prefix = funkin_prefix(glsl_ver, shader_prefix)
    frag = expand_funkin_fragment(frag_raw if frag_raw is not None else templates['defaultFragmentSource'],
                                  templates, assets_dir)
    vert = expand_funkin_vertex(vert_raw if vert_raw is not None else templates['defaultVertexSource'],
                                templates, assets_dir)
    return {
        'vertex': prefix + vert_prefix + vert,
        'fragment': prefix + frag_prefix + frag,
    }


def iter_asset_shaders(assets_dir):
    """Every `assets/shaders/**/*.frag|.vert` file (mods use the same loader)."""
    shaders_dir = os.path.join(assets_dir, 'shaders')
    if not os.path.isdir(shaders_dir):
        return
    for dirpath, dirnames, filenames in os.walk(shaders_dir):
        dirnames.sort()
        for fn in sorted(filenames):
            if not fn.endswith(('.frag', '.vert')):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, shaders_dir)
            yield rel, path


# ---------------------------------------------------------------- hashing
#
# The runtime (`lime.graphics.metal.ShaderAot`) implements the exact same two
# 32-bit hashes.  Keep the two in sync: the cache is keyed by them.

def fnv1a(text):
    h = 0x811C9DC5
    for byte in text.encode('utf-8'):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def djb2(text):
    h = 5381
    for byte in text.encode('utf-8'):
        h = (((h << 5) + h) & 0xFFFFFFFF) ^ byte
    return h


def key_of(source, is_vertex):
    payload = ('V' if is_vertex else 'F') + '\x00' + source
    return '%08X%08X' % (fnv1a(payload), djb2(payload))


_WS_RE = re.compile(r'\s+')
_IDENT_RE = re.compile(r'[^A-Za-z0-9_\s]')


def normalize(source):
    """Mirror of `ShaderAot.normalize` (whitespace/comment-insensitive cache key)."""
    text = re.sub(r'/\*.*?\*/', ' ', source, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)
    text = _WS_RE.sub(' ', text).strip()
    # drop spaces around punctuation so `vec4 (x)` == `vec4(x)`
    text = re.sub(r'\s*([^A-Za-z0-9_\s])\s*', r'\1', text)
    return text


# ------------------------------------------------------------- classpaths

# Libraries whose `@:gl*` metadata and asset shaders are known to reach the
# translator.  `.haxelib` is pinned per project (see libs.xml), `source` is the
# engine itself, and `~/haxelib-dev/lime-832` is the dev Lime checkout.
HAXELIB_DIRS = ['openfl', 'flixel', 'flixel-addons', 'flxanimate', 'away3d', 'lime', 'hscript-improved']


def default_roots(project_dir):
    roots = []
    haxelib = os.path.join(project_dir, '.haxelib')
    for name in HAXELIB_DIRS:
        base = os.path.join(haxelib, name)
        if not os.path.isdir(base):
            continue
        for entry in ('git', 'src'):
            candidate = os.path.join(base, entry)
            if os.path.isdir(candidate):
                roots.append(candidate)
                break
        else:
            # versioned haxelib: .haxelib/<name>/<version>[/src]
            for version in sorted(os.listdir(base)):
                candidate = os.path.join(base, version)
                if not os.path.isdir(candidate):
                    continue
                src = os.path.join(candidate, 'src')
                roots.append(src if os.path.isdir(src) else candidate)
    roots.append(os.path.join(project_dir, 'source'))
    return roots


def roots_from_hxml(project_dir):
    """Prefer the real release classpath when a build already exists."""
    hxml = os.path.join(project_dir, 'export', 'release', 'macos', 'haxe', 'release.hxml')
    if not os.path.exists(hxml):
        return None
    roots = []
    with open(hxml, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            line = line.strip()
            if line.startswith('-cp '):
                path = line[4:].strip()
                if os.path.isdir(path):
                    roots.append(path)
    return roots or None


# -------------------------------------------------------------- enumeration

class AotSource(object):
    """One shader stage that has to exist in the AOT cache."""

    def __init__(self, ident, kind, stage, source):
        self.ident = ident
        self.kind = kind          # 'class' | 'asset' | 'mod'
        self.stage = stage        # 'vertex' | 'fragment'
        self.source = source
        self.key = key_of(source, stage == 'vertex')
        self.norm = key_of(normalize(source), stage == 'vertex')


def collect_metadata_sources(classes, by_full, by_name):
    out = []
    for cls in classes:
        is_funkin = is_funkin_shader(cls, by_full, by_name)
        if is_funkin:
            prefix = funkin_prefix()
        else:
            prefix = openfl_prefix()
        combos = expand_class(cls, by_full, by_name)
        for index, combo in enumerate(combos):
            label = '%s#%d' % (cls.full, index)
            if combo['vertex'] is not None:
                out.append(AotSource('class:' + label + '/vertex', 'class', 'vertex', prefix + combo['vertex']))
            if combo['fragment'] is not None:
                out.append(AotSource('class:' + label + '/fragment', 'class', 'fragment', prefix + combo['fragment']))
        if not combos:
            sys.stderr.write('warn: class %s declares no usable GLSL\n' % cls.full)
    return out


def collect_asset_sources(project_dir, templates):
    """`assets/shaders/**` (and any mod shader files that are present at build time)."""
    assets_dir = os.path.join(project_dir, 'assets')
    if not os.path.isdir(assets_dir):
        return []

    # group .frag / .vert by shader name (CustomShader looks both up)
    pairs = {}
    for rel, path in iter_asset_shaders(assets_dir):
        stem, ext = os.path.splitext(rel)
        pairs.setdefault(stem, {})[ext] = path

    out = []
    for stem in sorted(pairs):
        files = pairs[stem]
        frag_raw = _read(files['.frag']) if '.frag' in files else None
        vert_raw = _read(files['.vert']) if '.vert' in files else None
        assembled = funkin_assembled(frag_raw, vert_raw, templates, assets_dir)
        out.append(AotSource('asset:assets/shaders/%s/vertex' % stem, 'asset', 'vertex', assembled['vertex']))
        out.append(AotSource('asset:assets/shaders/%s/fragment' % stem, 'asset', 'fragment', assembled['fragment']))

    # mods: same loader, files simply live below `mods/`
    mods_dir = os.path.join(project_dir, 'mods')
    if os.path.isdir(mods_dir):
        mod_pairs = {}
        for dirpath, dirnames, filenames in os.walk(mods_dir):
            dirnames[:] = [d for d in dirnames if not d.startswith('.')]
            for fn in sorted(filenames):
                if not fn.endswith(('.frag', '.vert')):
                    continue
                path = os.path.join(dirpath, fn)
                stem, ext = os.path.splitext(os.path.relpath(path, mods_dir))
                mod_pairs.setdefault('mods/' + stem, {})[ext] = path
        for stem in sorted(mod_pairs):
            files = mod_pairs[stem]
            frag_raw = _read(files['.frag']) if '.frag' in files else None
            vert_raw = _read(files['.vert']) if '.vert' in files else None
            assembled = funkin_assembled(frag_raw, vert_raw, templates, assets_dir)
            out.append(AotSource('mod:%s/vertex' % stem, 'mod', 'vertex', assembled['vertex']))
            out.append(AotSource('mod:%s/fragment' % stem, 'mod', 'fragment', assembled['fragment']))

    return out


def _read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        return fh.read()


def build_sources(project_dir, roots=None):
    roots = roots_from_hxml(project_dir) or roots or default_roots(project_dir)
    seen = set()
    unique = []
    for root in roots:
        root = os.path.abspath(root)
        if root in seen or not os.path.isdir(root):
            continue
        seen.add(root)
        unique.append(root)
    roots = unique
    classes, typedefs = collect_types(roots)
    by_full, by_name = make_index(classes, typedefs)
    templates = parse_shader_templates(os.path.join(project_dir, 'source/funkin/backend/shaders/FunkinShader.hx'))
    sources = collect_metadata_sources(classes, by_full, by_name)
    sources += collect_asset_sources(project_dir, templates)
    return sources, classes, roots
