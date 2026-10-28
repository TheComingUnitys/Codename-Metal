# Lime / OpenFL Haxe hooks for the Metal (bgfx) backend

This document describes the patches installed by
[`metal/scripts/patch_lime_haxe.py`](../../../../scripts/patch_lime_haxe.py) and *why* each one
exists.

The design rule of the Metal backend is that **nothing above Lime changes**:
`lime.graphics.opengl.GL`, OpenFL and HaxeFlixel keep calling exactly the same entry
points.  Only the four places where Lime hands GL work to the native layer are hooked
with `#if metal`, and every hook forwards to
[`lime.graphics.metal.MetalShaderBridge`](MetalShaderBridge.hx), which

1. remembers the shader stage (`createShader`),
2. translates the assembled GLSL into MSL with [`GLSLToMSL`](GLSLToMSL.hx) (`shaderSource`),
3. remembers OpenFL's attribute locations (`bindAttribLocation`) and attached shaders
   (`attachShader`),
4. after linking, pushes the `glLocation → bgfx a_*` map and the uniform table to the
   native backend with the `lime_metal_configure_program` primitive, and
5. drops the bookkeeping again (`deleteShader` / `deleteProgram`).

Everything the engine draws — sprites, vector graphics, `FlxShader`s — goes through
`GL`, so routing those five calls is enough to move the whole renderer to Metal.

## Running the patcher

```bash
python3 metal/scripts/patch_lime_haxe.py \
    --lime   /path/to/lime \
    --openfl /path/to/openfl
```

* Idempotent — every hunk carries a unique `[cne-metal]` marker; a second run reports
  “already done” and writes nothing (verified: all `.hx` checksums identical after run 2).
* Mandatory anchors that do not match **fail loudly**: the file path, the exact expected
  anchor and the surrounding lines are printed and the script exits `1`.
* `--openfl-missing {warn,error,skip}` — what to do when
  `<openfl>/src/openfl/utils/GLSLSourceAssembler.hx` is absent (default `warn`: print a
  `SKIP` block, try the `Shader.hx` fallback, exit `0`).
* `--dry-run` — report without writing.

The Haxe build must define `metal` (the project does this in `Project.xml`, right next to
`<define name="metal"/>` for the native side).  Without `-D metal` **none** of the hooks
below are compiled, so the patch is inert unless the define is present.

---

## 1. `src/lime/_internal/backend/native/NativeCFFI.hx` — the two bridge primitives

| # | anchor (exact) | inserted | patched line |
|---|----------------|----------|--------------|
| 1a | `@:cffi private static function lime_lzma_decompress(...)` + `lime_mouse_event_manager_register` (cpp declarations) | `@:cffi` declarations | `NativeCFFI.hx:239` |
| 1b | `private static var lime_lzma_decompress = new cpp.Callable<...>("ooo", false)` + `lime_mouse_event_manager_register` (cpp loaders) | `cpp.Callable` loaders | `NativeCFFI.hx:556` |
| 1c | `private static var lime_lzma_decompress = CFFI.load(...)` + `lime_mouse_event_manager_register` (neko) | `CFFI.load` | `NativeCFFI.hx:783` |
| 1d | `@:hlNative("lime", "hl_lzma_decompress")` + `hl_mouse_event_manager_register` (hl) | `@:hlNative` | `NativeCFFI.hx:1223` |

```haxe
#if metal
@:cffi private static function lime_metal_configure_program(program:Int, attributeMap:Array<Dynamic>, uniformMap:Array<Dynamic>):Dynamic;
@:cffi private static function lime_metal_is_metal():Int;
#end
```

All three targets are covered because Lime has three separate FFI tables:

* **cpp** — `cpp.Prime._loadPrime("lime", name, signature, false)`,
* **neko** — `CFFI.load("lime", name, argCount)` (degrades to a `Reflect` stub when the
  ndll does not export the primitive),
* **hl** — `@:hlNative("lime", "hl_" + name)`, following the existing `hl_gl_*` entries.

### Signature: `"iooo"`, and the Haxe type is `Dynamic`, not `Bool`

This is worth spelling out because it is easy to get wrong:

* the native implementation in `metal/native/MetalBindings.cpp` is
  `value lime_metal_configure_program(value programId, value attributeMap, value uniformMap)`
  and is registered with `DEFINE_PRIME3v(lime_metal_configure_program)`
  (`MetalBindingsRegister.inc`); `lime_metal_is_metal()` returns `int`.
* `cpp.Prime._loadPrime` hands the signature to `func__prime`, i.e. to
  `cffi::CheckSig3` in hxcpp's `include/hx/CFFIPrime.h`.  That check compares the
  **actual C++ types**: `SigType<value>::Char == 'o'`, `SigType<int>::Char == 'i'`,
  `SigType<Dynamic>` → `cpp.Object` → `'o'`.
* so the signature is **`"iooo"`** (`i`, `o`, `o`, return `o`) and the Haxe side must be
  `Int -> cpp.Object -> cpp.Object -> cpp.Object`.  A `:Bool` declaration / `"ioob"`
  would make `_loadPrime` throw `lime_metal_configure_program does not have signature ioob`
  at class init (and `cpp::Function<T>` is a raw function pointer, so a mismatched return
  type is an ABI error, not just a type error).
* the signature is modelled on `lime_lzma_decompress(data:Dynamic, bytes:Dynamic):Dynamic`
  (declaration + `cpp.Callable<cpp.Object->cpp.Object->cpp.Object>` `"ooo"`) and
  `lime_gl_bind_attrib_location(program:Int, index:Int, name:String)` `"iisv"`;
  `lime_metal_is_metal():Int` mirrors `lime_system_get_num_displays()` (`Void->Int`, `"i"`).
* cross-check: Lime's own `CFFIMacro.__getFunctionType` maps any non-primitive type
  (`Dynamic`, `Array<Dynamic>`, …) to `'o'`, so the `@:cffi` declarations for the legacy
  `disable_cffi` / `haxe_ver < 3.4.0` path generate `"iooo"` as well.
* verified: compiling the patched Lime with `-D metal` generates
  `_loadPrime("lime", "lime_metal_configure_program", "iooo", false)` and
  `::cpp::Function< int () > lime_metal_is_metal`.

`MetalShaderBridge.onLinkProgram` intentionally ignores the (always-`null`, see
`DEFINE_PRIME3v`) return value; `isMetal()` is the only caller that uses
`lime_metal_is_metal`.

## 2. `src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx` — the five hooks

| hook | anchor | patched line |
|------|--------|--------------|
| import | after `import lime.utils.UInt32Array;` | `:29` |
| `createShader(type)` | after `var id = NativeCFFI.lime_gl_create_shader(type); if (id == 0) return null;` | `:1104` |
| `shaderSource(shader, source)` | before `NativeCFFI.lime_gl_shader_source(...)` | `:2811` |
| `attachShader(program, shader)` | before `NativeCFFI.lime_gl_attach_shader(...)` | `:690` |
| `bindAttribLocation(program, index, name)` | before `NativeCFFI.lime_gl_bind_attrib_location(...)` | `:717` |
| `linkProgram(program)` | after `NativeCFFI.lime_gl_link_program(...)` | `:2672` |
| `deleteShader(shader)` | before `NativeCFFI.lime_gl_delete_shader(...)` | `:1221` |
| `deleteProgram(program)` | before `NativeCFFI.lime_gl_delete_program(...)` | `:1184` |

Notes:

* the hooks use the same object ids as the native calls (`__getObjectID(...)`,
  `id` from `createShader`), so bridge bookkeeping and backend state stay aligned;
* `shaderSource` reassigns the `source` parameter (`source = MetalShaderBridge.onShaderSource(...)`),
  which Haxe allows;
* they live *inside* the existing `#if (lime_cffi && (lime_opengl || lime_opengles) && !macro)`
  block, so they only exist when the native OpenGL backend is actually being used;
* `linkProgram` calls the bridge **after** the native link, because
  `onLinkProgram` needs both shaders attached and the GL attribute locations recorded.

## 3. `<openfl>/src/openfl/utils/GLSLSourceAssembler.hx` — force legacy GLSL

| hook | anchor | patched line |
|------|--------|--------------|
| `getDefaultVersion()` | `#if (web || mobile)` + `return "300 es";` | `:20` |
| `getDefaultLegacyVersion()` | `#if (web || mobile || mac)` + `return "100";` | `:39` |

```haxe
#if metal
// ... why ...
return "120";
#elseif (web || mobile)
...
```

macOS asks for `"410 core"` (and `"100"` for legacy shaders); the Metal path translates
only the legacy 100/120 dialect (see the `GLSLToMSL` header comment and
`Flags.DEFAULT_GLSL_VERSION == "120"`).  Pinning both functions to `"120"` under
`#if metal` means the translator only ever sees one dialect, while every other platform
keeps its previous behaviour (verified by compiling the two functions in isolation:
mac without `-D metal` → `410 core` / `100`, with `-D metal` → `120` / `120`).

### Important: this file does not exist in every OpenFL checkout

`.haxelib/openfl/git` is currently checked out at `d97b17208` (2025-08-07), which predates
`openfl/utils/GLSLSourceAssembler.hx` entirely — that revision assembles shader prefixes in
`openfl/display/Shader.hx` and emits **no** `#version` directive at all.
`GLSLSourceAssembler.hx` exists on `origin/cne` (`68643ef43`) and `origin/develop`
(`8e02ae82`) with identical `getDefaultVersion()` / `getDefaultLegacyVersion()` bodies, and
the same script patches those checkouts unchanged.

* the patcher therefore treats a missing file as **non-fatal** by default
  (`--openfl-missing=warn`): it prints a `SKIP` block with the reason and tries hunk 4;
* `--openfl-missing=error` makes it exit `3` instead (for CI that must not silently skip).

## 4. (fallback) `<openfl>/src/openfl/display/Shader.hx`

Only attempted when hunk 3 is unavailable.  Older OpenFL revisions build the GLSL prefix
in `Shader.hx`; the first matching anchor of

1. a `"#version …"` string literal,
2. the mac `"410 core"` string literal, or
3. a `GLSLSourceAssembler.getDefaultVersion()` call site

is wrapped in an **inline** conditional:

```haxe
return (#if metal "#version 120\n" /* [cne-metal] Shader.hx GLSL version -> 120 */ #else "#version 410 core\n" #end) + "…";
```

Haxe accepts `#if` in expression position (verified with and without `-D metal`).  If none
of the anchors is found the hunk `SKIP`s with the list of searched patterns — it never
fails the run.  On the current `d97b17208` checkout it always SKIPs (no anchor and no
`#version` in the file), which is why hunk 3 is the load-bearing one.

---

## Verification performed

```
$ python3 scripts/patch_lime_haxe.py --lime /tmp/…/lime --openfl /tmp/…/openfl
summary: 14 patched, 0 already applied, 1 skipped, 0 failed          (exit 0)

$ python3 scripts/patch_lime_haxe.py --lime /tmp/…/lime --openfl /tmp/…/openfl   # 2nd run
summary: 0 patched, 14 already applied, 1 skipped, 0 failed          (exit 0)
# sha256 of all 411 lime .hx + 435 openfl .hx files identical after run 2
```

* drifted anchors / missing files → `[FAIL]` block with file, expected anchor and nearest
  context, exit `1` (tested with a deliberately renamed `lime_lzma_decompress`).
* legacy OpenFL checkout (no `GLSLSourceAssembler.hx`) → 2 `SKIP` + 1 `SKIP`, exit `0`.
* patched Lime compiles with `-D metal -D lime_cffi -D lime_opengl` (`haxe -cpp … -D no-compilation`
  ⇒ exit `0`) once the two missing imports below are present.

## Known compile blockers outside this patcher (need a fix in the shim)

`metal/haxe/lime/graphics/metal/MetalShaderBridge.hx` as committed does **not** compile;
`@:access(...)` does not import anything, so it is missing two imports:

```haxe
import lime.graphics.metal.MetalTypes.TranslateResult;   // used on lines 30 / 126
import lime._internal.backend.native.NativeCFFI;         // used on lines 182 / 188
```

With those two lines added (tested in a scratch copy only), `haxe -cpp … -D metal` for the
patched Lime exits `0`.  Without them every hook fails with
`Type not found : TranslateResult` / `Type not found : NativeCFFI`.  This file is *not*
owned by `patch_lime_haxe.py` — fix it in `metal/haxe/lime/graphics/metal/`.

## Reverting

The patch is additive: delete every block that contains `[cne-metal]` (they are each
wrapped in `#if metal … #end`), or start from a fresh Lime/OpenFL checkout and re-run
`metal/scripts/apply.sh`.
