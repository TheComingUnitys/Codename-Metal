# Lime 8.3.2 Haxe patches (Metal backend)

**English** | [中文](zh-CN/LIME-PATCHES.md)

The Metal/bgfx backend needs hooks in Lime's Haxe CFFI/SDL path. They are applied
idempotently by `CodenameEngine-1.0.1/metal/scripts/patch_lime_haxe.py` (called by
`apply.sh`). The same set is exported as an upstreamable patch:

- `metal/patches/lime-metal.patch` - Haxe patch against **openfl/lime 8.3.2**
  (11 files / 13 changes);
- `metal/patches/lime-metal.patch.sha256` - checksum.

> `patch_lime_haxe.py` also lists three optional OpenFL-side candidates (GLSL version 120),
> but this project does not modify OpenFL sources (`.haxelib/openfl/git` has no `src/`),
> so they are not exported.

## Applying

A normal build needs no manual patch step:

```sh
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --build --deploy
```

`apply.sh` invokes `patch_lime_haxe.py` (idempotent; safe to re-run).

## Regenerating the patch

```sh
# pristine upstream checkout
git clone --depth 1 --branch 8.3.2 https://github.com/openfl/lime /tmp/lime-pristine

./metal/scripts/export-lime-patches.sh \
  --lime "$HOME/haxelib-dev/lime-832" \
  --pristine /tmp/lime-pristine \
  --out metal/patches --verify
```

`--verify` applies the generated patch to a copy of the pristine tree and diffs every file
against the live patched checkout; it only passes when all files match.

## Upgrading Lime

1. update `~/haxelib-dev/lime-832` to the target version;
2. re-run `patch_lime_haxe.py` (anchored patches; a moved anchor fails with context);
3. bump the pristine version used by `export-lime-patches.sh` and regenerate the patch;
4. run `./verify.sh --with-native` and, when building, one game launch to confirm no regression.

## Upstreaming

`lime-metal.patch` is a standard `git apply` patch and can be sent as a PR to openfl/lime.
It contains three kinds of changes:

1. `NativeCFFI.hx`: `cne_metal_*` / `lime_metal_*` CFFI declarations for the backend;
2. `NativeOpenGLRenderContext.hx`: shader create/compile/link hooks (GLSL goes to the
   AOT cache/translator, MSL goes to bgfx);
3. GLSL version changes (`#version` 120) when enabled.

After touching these files, re-export the patch and mention the Lime version in the commit.
