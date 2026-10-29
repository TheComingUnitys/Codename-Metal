# Lime 8.3.2 Haxe 补丁（Metal 后端）

Metal/bgfx 后端需要 Lime 的 Haxe CFFI/SDL 路径配合，补丁由
`CodenameEngine-1.0.1/metal/scripts/patch_lime_haxe.py` 幂等应用（`apply.sh` 会自动调用）。
本目录把它们导出为可上游化的统一补丁：

- `metal/patches/lime-metal.patch` — 相对 **openfl/lime 8.3.2** 的 Haxe 补丁（11 个文件 / 13 处改动）；
- `metal/patches/lime-metal.patch.sha256` — 校验和。

> OpenFL 侧另有 3 处候选补丁（GLSL 版本 120）登记在 `patch_lime_haxe.py` 里，
> 但当前工程不修改 OpenFL 源码（`.haxelib/openfl/git` 中没有 `src/`），因此没有导出。

## 应用

正常构建不需要手动打补丁：

```sh
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --build --deploy
```

`apply.sh` 调用 `patch_lime_haxe.py`（幂等，可重复运行）。

## 重新生成补丁

```sh
# 取一份未打补丁的上游源码
git clone --depth 1 --branch 8.3.2 https://github.com/openfl/lime /tmp/lime-pristine

./metal/scripts/export-lime-patches.sh \
  --lime "$HOME/haxelib-dev/lime-832" \
  --pristine /tmp/lime-pristine \
  --out metal/patches --verify
```

`--verify` 会把生成的补丁应用到 pristine 副本，并逐文件与当前已打补丁的 Lime 对比；
全部一致才通过。

## 升级 Lime 的步骤

1. 把 `~/haxelib-dev/lime-832` 更新到目标版本（新的 dev checkout）；
2. 重新跑 `patch_lime_haxe.py`（补丁按锚点匹配；锚点失效会报错并给出上下文）；
3. 更新 `export-lime-patches.sh` 里记录的 pristine 版本并重新生成补丁；
4. 跑 `./verify.sh --with-native` 与（可构建时）一次游戏启动，确认无回归。

## 上游化

`lime-metal.patch` 是标准 `git apply` 格式，可直接用于给 openfl/lime 提 PR。
补丁内容分三类：

1. `NativeCFFI.hx`：Metal 后端的 `cne_metal_*` / `lime_metal_*` CFFI 声明；
2. `NativeOpenGLRenderContext.hx`：shader 创建/编译/链接钩子（把 GLSL 交给 AOT/翻译器，
   MSL 再交给 bgfx）；
3. `GLSLSourceAssembler.hx`-类改动（若启用）：`#version` 120。

修改这些文件后请重新导出补丁，并在提交信息里写明 Lime 版本。
