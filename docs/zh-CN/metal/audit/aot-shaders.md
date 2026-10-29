# Shader AOT（构建期 GLSL→MSL）审计：运行时零翻译

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/audit/aot-shaders.md)

> 目标：**消除运行时 GLSL→MSL 翻译**，让每个 shader 的 Metal 源在构建期就确定。
> 对象：`CodenameEngine-1.0.1` + bgfx v166 Metal 后端 + `metal/` 里的 GLSL→MSL 翻译器。
> 前序审计：[`shaders.md`](shaders.md)（运行时链路、5 个 program 的实测数据）。
> 日期：2026-10-17。

---

## 0. 结论（TL;DR）

1. **运行时不再有任何 GLSL→MSL 翻译。** `MetalShaderBridge.onShaderSource` 现在只做
   `hash(源) → ShaderAotCache` 查表；命中就直接把预生成的 MSL 交给原生后端。
   计数器 `GLSLToMSL.runtimeCallCount` 在真实运行中 = **0**（见 §6）。
2. **缓存由构建期自动生成，无手动步骤。** `metal/scripts/apply.sh` 每次构建前运行
   `metal/scripts/gen_shader_aot.py`：枚举全部 GLSL → 用**同一个** `GLSLToMSL`
   （`haxe --interp`，不需要 C++ 工具链）预翻译 → 生成被编进 app 的 Haxe 常量
   `lime/graphics/metal/ShaderAotCache.hx`。
3. **覆盖 100% 可静态枚举的 shader**：56 个 stage（15 vertex + 41 fragment），
   含 OpenFL 内建 3 个、12 个 filter、flxanimate、CNE `OverlayShader`/`WiggleShader`、
   `assets/shaders/**` 全部 12 个 `.frag` + 1 个共享 vertex。
   运行时真实用到的 5 个 program 的 10 个 stage **全部命中**。
4. **正确性有逐字节证据**：把生成缓存里的 MSL 与 `shaders.md` 审计时落盘的
   `/tmp/cne-shader-0..9.metal`（运行时真正编译过的那 10 份）对比，**10/10 完全一致**。
   抽取算法不是"看起来对"，而是可证伪地一致。
5. **零回归**：标题→主菜单→Options→CONTROLS 跑通；3 个内建 program 全部
   `origin=AOT-cache`、`compileFailures=0`、`submit` 正常；干净退出（exit 0）。
6. **未命中 = 响亮报错**，绝不静默回退：打印 cache key + stage + 完整 GLSL +
   重新生成命令，把源码写到 `/tmp/cne-aot-missing/<key>.<stage>.glsl`，
   并把原 GLSL 交回去让 bgfx/Metal 编译失败（该 program 不出图，日志里有原因）。
   唯一的例外是显式开关 `CNE_METAL_AOT_RUNTIME_TRANSLATE=1`（见 §8）。

---

## 1. 改动前的链路 vs 改动后

```
改动前（shaders.md §3）
  OpenFL/Flixel/CNE 组装 GLSL
    → gl.shaderSource          ┐
    → MetalShaderBridge        │  运行时翻译
         GLSLToMSL.translateSafe(GLSL)   ← 每个 shader 每次启动都翻译一遍
    → lime_gl_shader_source（保存 MSL）
    → lime_gl_compile_shader → WrapShaderBin → bgfx::createShader
    → Metal newLibraryWithSource（运行时编译 MSL，这一步无法消除：无 Xcode metal 工具链）

改动后
  构建期（apply.sh，每次构建）
    gen_shader_aot.py
      1. 枚举所有 GLSL（源码 metadata 宏展开 + assets/mods 文件）
      2. 用同一个 GLSLToMSL 翻译 → 56 个 stage
      3. 生成 ShaderAotCache.hx（hash → MSL + attribute/uniform/sampler 表）
    → 随 Haxe 编译进 app 二进制

  运行时
    OpenFL/Flixel/CNE 组装 GLSL（这步是 OpenFL 本来就做的，不是翻译）
    → gl.shaderSource
    → MetalShaderBridge.onShaderSource
          hash = ShaderAot.hash(source, stage)
          entry = ShaderAot.fetch(...)        ← 只查表，0 次翻译
    → lime_gl_shader_source（保存 MSL）
    → lime_gl_compile_shader → WrapShaderBin → bgfx::createShader
    → Metal newLibraryWithSource（bgfx 内部把 MSL 编成 MTLLibrary；与 AOT 无关）
```

> 说明：本文档只覆盖 **GLSL→MSL 翻译**这一步。**MSL→metallib** 的构建期编译
> （运行时 `newLibraryWithSource` 归零）在 [`aot-coverage.md`](../../../../CodenameEngine-1.0.1/metal/audit/aot-coverage.md) 里，
> 两条链路是同一次 `gen_shader_aot.py` 构建期跑完的（§9.3）。

---

## 2. 新增/改动的文件

| 文件 | 作用 |
| --- | --- |
| `metal/scripts/gen_shader_aot.py` | 构建期 AOT 生成器（枚举 → 翻译 → 写缓存 + 覆盖率报告） |
| `metal/scripts/aot_shaders.py` | 枚举/抽取库：复现 OpenFL `ShaderMacro` 宏展开、`FunkinShader` 的 `#pragma` 展开、typedef 继承解析、hash/normalize |
| `metal/tools/GenShaderAot.hx` | `haxe --interp` 入口：读 manifest → 调 `GLSLToMSL` → 写结果 JSON |
| `metal/haxe/lime/graphics/metal/ShaderAot.hx` | 运行时：hash / normalize / 查表 / 计数器 / 未命中处理 / 退出报告 |
| `metal/haxe/lime/graphics/metal/ShaderAotCache.hx` | **生成物**：hash → MSL + attribute/uniform/sampler 表（JSON 常量，一次解析） |
| `metal/haxe/lime/graphics/metal/MetalShaderBridge.hx` | `onShaderSource` 改为查表；注册退出报告 |
| `metal/haxe/lime/graphics/metal/GLSLToMSL.hx` | 新增 `runtimeCallCount` 计数器（运行时调用次数必须是 0） |
| `metal/native/MetalBindings.cpp` | `ShaderData.aotSource` + `[cne-shader-audit] MSL ... origin=AOT-cache|runtime-translated key=…` |
| `metal/scripts/apply.sh` | 新增步骤 1b（生成缓存）与 `--skip-aot` / `--deploy` |
| `metal/audit/aot-coverage.md` | **生成物**：56 条缓存的逐条清单（来源 / stage / key / 大小） |

`apply.sh` 会把 `ShaderAot.hx` / `ShaderAotCache.hx` 一起同步到
`<lime>/src/lime/graphics/metal/` 与 `CodenameEngine-1.0.1/source/lime/graphics/metal/`
（与其它 shim 相同路径），所以**构建不需要任何手动步骤**。

---

## 3. 步骤 1：枚举全部 GLSL 来源

### 3.1 方法

三条独立来源，缺一不可：

1. **`@:gl*` metadata 类**（OpenFL `ShaderMacro` 宏展开）
   `openfl.utils._internal.ShaderMacro.build()` 在编译期把
   `@:glVertexHeader/glVertexBody/glVertexSource/glFragmentHeader/glFragmentBody/glFragmentSource`
   拼成 `__glVertexSource` / `__glFragmentSource` 两个**编译期常量**。
   `aot_shaders.py` 从 `.hx` 源码里按同样规则重建：
   - 解析 Haxe 字符串字面量（含多行字符串、转义、单/双引号）；
   - 用**保留字节偏移**的注释剥离（否则 metadata 会归属到错误的类——这是实际踩到的坑）；
   - 沿 `extends` 链聚合（派生类优先赋值；祖先的 header/body 逐个 **前置**拼接）；
   - `#pragma header` / `#pragma body` 替换；
   - `typedef` 别名解析（`FlxShader = flixel.graphics.tile.FlxGraphicsShader`，
     不解析就会丢掉整个 flixel header → `bitmap` 未声明 → 翻译失败）；
   - 源码在两个 `#if/#else` 分支各出现一次时（`#if emscripten`）**两个变体都进缓存**（超集无害）。
2. **CNE `CustomShader` 的资产 GLSL**：`assets/shaders/**/*.frag|*.vert`。
   走的是 `FunkinShader` 的运行时路径：`#import <...>` 展开
   （`Assets.getText("assets/shaders/" + path)`）、`#pragma header` → `ShaderTemplates.fragHeader`、
   `#pragma body` → `ShaderTemplates.fragBody`、vertex 侧还有 `useBackCompat` 分支
   （`vertHeader` vs `vertHeaderBackCompat`）。
   `ShaderTemplates` 的常量**从 `FunkinShader.hx` 源码解析**得到（不在生成器里硬编码，
   避免双份真源漂移）。
3. **`mods/**` 里的 `.frag|.vert`**（构建时存在的 mod 也算进来；见 §8 关于运行时新增 mod）。
4. 前缀（`Shader.__initGL` / `FunkinShader.__initGL` 拼的 `#version` + 精度块）按两个基类分别复现，
   逐字节与运行时一致（见 §5 的 10/10 证据）。

**classpath 来源**：优先复用 `export/release/macos/haxe/release.hxml` 里真实的 `-cp` 列表
（即构建实际用的那份），没有构建产物时回退到 `.haxelib/*` + `source`。
所以枚举的库集合与真实构建完全一致，而不是猜。

### 3.2 覆盖清单

| 来源类别 | 具体 | stage 数 |
| --- | --- | --- |
| OpenFL 内建 | `DisplayObjectShader`、`GraphicsShader`（openfl 与 CNE 两份实现各一，超集） | 6 |
| Flixel | `FlxGraphicsShader`（+ 经 typedef 继承的 CNE `OverlayShader`、`WiggleShader`） | 6 |
| OpenFL filters（12 个 shader） | `BitmapFilterShader`、`BevelShader`、`BlurShader`、`ColorMatrixShader`、`ConvolutionShader`、`DisplacementMapShader`、`HideShader`、`InvertAlphaShader`、`BlurAlphaShader`、`CombineShader`、`InnerCombineShader`、`CombineKnockoutShader`、`InnerCombineKnockoutShader` | 26 |
| OpenFL mask（死代码，仍进缓存） | `Context3DMaskShader`、`Context3DAlphaMaskShader` | 4 |
| flxanimate | `FilterShader`（+ 变体） | 4 |
| CNE 资产 shader | `assets/shaders/*.frag`（3 个）+ `assets/shaders/engine/*.frag`（9 个）+ 共享 vertex | 13 |
| **合计** | | **56**（15 vertex + 41 fragment，见 `aot-coverage.md`） |

> 说明：`flixel/effects/postprocess/Shader.hx` 编译期被 `#if FLX_POST_PROCESS` 排除
> （构建无该 define），枚举不到也不需要；`lime` 侧没有 GLSL 声明。
> `OverlayShader`/`WiggleShader` 表面 `extends FlxShader`，而 `FlxShader`
> 是 `FlxGraphicsShader` 的 typedef —— 所以它们的 `#pragma header` 会用 flixel 的完整
> header（含 `bitmap`、`flixel_texture2D` 等），这是靠 typedef 解析还原的。

---

## 4. 步骤 2：构建期工具与缓存格式

```
python3 metal/scripts/gen_shader_aot.py [--project DIR] [--out FILE] [--check]
```

执行内容：

1. 枚举（§3）→ 去重（按 `hash`），得到 56 个 stage 的 GLSL；
2. 写 JSON manifest → `haxe -cp metal/haxe -cp metal/tools --run GenShaderAot <manifest> <out>`
   （`--interp`，用**同一份 `GLSLToMSL`** 翻译，失败会明确列出 source 标识并以非零码退出）；
3. 生成 `ShaderAotCache.hx`：

```haxe
package lime.graphics.metal;
class ShaderAotCache {
    public static inline var COUNT:Int = 56;
    public static inline var FINGERPRINT:String = "5C3E7B9A012E7E30";
    public static var DATA(default, null):String = "{...}" // JSON: [{k,n,s,m,z,a,u,t}...]
}
```

   - `k` = 精确 key，`n` = 归一化 key，`s` = stage，`m` = MSL（带 `// [cne-aot] key=…` 溯源标记），
     `z/a/u/t` = uniformSize / attributes / uniforms / samplers —— 即
     `MetalShaderBridge.onLinkProgram` 建 attribute/uniform 表所需的全部信息；
   - 缓存是 **JSON 常量字符串 + 首次使用时 `haxe.Json.parse` 一次**，避免在生成器里拼几十 KB
     的 Haxe 对象字面量（转义更容易出错），也避免把 56 份结构体写成难 review 的代码；
   - 输出是**确定性**的（排序 + 内容指纹），内容不变则不重写文件 → 不会每次构建都脏 git / 触发重编。
4. 写 `metal/audit/aot-coverage.md`（逐条覆盖率表）。

`apply.sh` 里的接入（**自动，无需手动步骤**）：

```sh
if [ "$DO_AOT" = "1" ]; then
    python3 "$HERE/gen_shader_aot.py" --project "$PROJECT_DIR" || { echo "error: AOT shader cache generation failed"; exit 1; }
fi
```

（`--skip-aot` 可跳过，仅用于特殊排查；跳过时使用已提交的缓存。）

---

## 5. 步骤 3/4：运行时只查表 + 零翻译证明

### 5.1 key 的算法

`key = FNV-1a-32(stageByte + "\0" + UTF-8(source)) ++ DJB2-32(同一 payload)`，大写 8+8 位 hex。
`stageByte` 是 `'V'` / `'F'`，64 位 key 空间，56 条缓存碰撞概率可忽略。

`ShaderAot.normalize()` 另算一个**归一化 key**（去注释、折叠空白、去标点两侧空格）：
只差缩进/空白/注释的源码会命中同一条缓存（这类差异不改变 token 流，MSL 必然相同）。
作用是给"抽取结果与运行时源码只差格式化"留一个安全网——它**仍然是 AOT 查表，不是翻译**。

> **实现坑（值得记录）**：hash 必须用 `haxe.Int64` 算，不能用 `Int`。
> hxcpp 把 `Int` 编译成 C++ `int`，有符号溢出是 UB；clang -O2 会把 32 位 FNV 的循环
> **直接优化错**（实测：同一个循环，不带副作用返回时得到 `INT_MIN`，加一句打印就得到正确值）。
> 第一版 hash 就是这样让 6 个 stage 全部 miss（key 前半截变成 `80000000`）。
> 现在所有中间值都在 64 位内且 < 2^57，不溢出，结果与 Python 生成器逐位一致
> （已用 cpp target 实测 102/102 源码 key 相等）。

### 5.2 运行时行为

| 情况 | 行为 |
| --- | --- |
| 精确命中 | 直接用缓存 MSL，`hits++`；`CNE_METAL_AOT_AUDIT=1` 时打印 `[cne-aot] hit …` |
| 归一化命中 | 同上，`normalizedHits++`，打印 `hit (normalised)` |
| 未命中 | `misses++`，打印 §7 的错误块、落盘源码、**不翻译**，返回原 GLSL（→ bgfx 编译失败，该 program 不出图） |
| `CNE_METAL_AOT_RUNTIME_TRANSLATE=1` | 显式逃生门：允许运行时翻译，`runtimeTranslations++`，并打印警告。默认关闭 |

每次编译还会打印一行（`CNE_METAL_AOT_AUDIT=1`）：

```
[cne-aot] shader id=2 stage=vertex msl=AOT-cache bytes=2107 key=44FE5B3E28554890
```

原生侧（`MetalBindings.cpp`）识别 MSL 头部的 `[cne-aot] key=` 标记，审计输出区分来源：

```
[cne-shader-audit] MSL shader=2 type=0x8B31 bytes=2107 origin=AOT-cache key=44FE5B3E28554890
```

### 5.3 证明一：与审计时落盘的 MSL 逐字节相同

`shaders.md` 的会话 D 用 `CNE_METAL_DEBUG=1` 落盘了运行时真正编译的 10 份 MSL
（`/tmp/cne-shader-0..9.metal`）。把**生成缓存里对应 key 的 MSL**（去掉溯源注释行）
与它们对比：

| dump | 对应 program | 缓存 key | 字节一致 |
| --- | --- | --- | --- |
| 0 / 1 | 4 `DisplayObjectShader` | `44FE5B3E28554890` / `3E737C798B919375` | ✅ / ✅ |
| 2 / 3 | 8 `GraphicsShader` | `6C274AAEE7CE0290` / `8857E973D63833D5` | ✅ / ✅ |
| 4 / 5 | 14 `FlxGraphicsShader` | `E4874A95E5BDB0CB` / `C3575A6D068F0DB5` | ✅ / ✅ |
| 6 / 7 | 162 `coloredVignette` | `64629A8739C1BFAB` / `5A7E3F62A7ABC4F4` | ✅ / ✅ |
| 8 / 9 | 536 `pixelZoomShader` | `64629A8739C1BFAB`（共享 vertex）/ `6E59685876B1BA2E` | ✅ / ✅ |

→ 抽取出的 GLSL 与运行时组装的 GLSL **逐字节相同**（否则 MSL 不可能字节相同），
包括 2 个自定义 shader program。

补充（同机 cpp target 离线验证）：用 `-cpp -D analyzer-optimize` 编译一个测试程序，
链接真实的 `ShaderAot.hx` + `ShaderAotCache.hx`，对 102 个枚举源码逐一验证：
**102/102 hash 与 Python 一致、102/102 命中缓存、0 miss、`runtimeCallCount=0`**。

### 5.4 证明二：真实运行日志

`CNE_METAL_AOT_AUDIT=1 CNE_METAL_SHADER_AUDIT=1`，标题→主菜单→Options→CONTROLS（约 74k 笔 submit）：

```
[cne-aot] EXIT cache=5C3E7B9A012E7E30 entries=56 hits=6 normalizedHits=0 misses=0 \
          runtimeTranslations=0 runtimeGlslToMslCalls=0

[cne-shader-audit] MSL shader=2  type=0x8B31 bytes=2107 origin=AOT-cache key=44FE5B3E28554890
[cne-shader-audit] MSL shader=3  type=0x8B30 bytes=2455 origin=AOT-cache key=3E737C798B919375
[cne-shader-audit] MSL shader=6  type=0x8B31 bytes=2103 origin=AOT-cache key=6C274AAEE7CE0290
[cne-shader-audit] MSL shader=7  type=0x8B30 bytes=2411 origin=AOT-cache key=8857E973D63833D5
[cne-shader-audit] MSL shader=12 type=0x8B31 bytes=2446 origin=AOT-cache key=E4874A95E5BDB0CB
[cne-shader-audit] MSL shader=13 type=0x8B30 bytes=3567 origin=AOT-cache key=C3575A6D068F0DB5
```

**`runtimeGlslToMslCalls=0`** 即"运行时翻译器调用次数 = 0"，这是硬指标。

---

## 6. 步骤 5：画面零回归

同一次运行（日志 `/tmp/aot-final-controls.log`）：

```
[cne-shader-audit] EXIT programs=3 shaderCompileFailures=0 programLinkFailures=0 \
                  drawsWithoutProgram=0 uniformLocationMisses=0 shadersNotMsl=0
[cne-shader-audit] EXIT program=4  linked=1 bgfxProgram=10 vs=2  fs=3  submits=20460 uniforms=4 attribs=5
[cne-shader-audit] EXIT program=8  linked=1 bgfxProgram=11 vs=6  fs=7  submits=5886  uniforms=4 attribs=5
[cne-shader-audit] EXIT program=14 linked=1 bgfxProgram=12 vs=12 fs=13 submits=47951 uniforms=7 attribs=8
```

* 3 个内建 program 全部 `linked=1`、attribute/uniform 表与 `shaders.md` 完全一致（4/4、4/4、6/7）；
* `shaderCompileFailures=0`、`programLinkFailures=0`、`drawsWithoutProgram=0`、`shadersNotMsl=0`；
* `bgfx renderer: Metal (Metal: yes), 2498x1440, high-dpi`；
* 干净退出（exit code 0，无 `Abort trap`、无 `bgfx fatal`）；
* 全程 `grep -cE "NOT AHEAD|NOT-MSL|COMPILE-FAILED|LINK-FAILED|bgfx fatal"` = **0**。

### 6.1 自定义 shader（`CustomShader`）的运行时证据

另一次运行（标题→主菜单→Freeplay→进歌，日志 `/tmp/aot-final-song.log`）在歌曲脚本
`assets/data/scripts/pixel.hx` 里创建了 `CustomShader('pixelZoomShader')` —— 即
`shaders.md` 的 **program 536**。另一次 45 秒的 smoke 运行（重新构建后的最终二进制，日志 `/tmp/aot-final-smoke.log`）同样是
`hits=6 misses=0 runtimeTranslations=0 runtimeGlslToMslCalls=0`、`bgfx fatal` 0 次、退出码 0。
歌曲运行日志：

```
[cne-aot] hit shader stage=vertex key=64629A8739C1BFAB
[cne-aot] shader id=178 stage=vertex   msl=AOT-cache bytes=2441 key=64629A8739C1BFAB
[cne-aot] hit shader stage=fragment key=6E59685876B1BA2E
[cne-aot] shader id=179 stage=fragment msl=AOT-cache bytes=3702 key=6E59685876B1BA2E
...
[cne-aot] EXIT cache=5C3E7B9A012E7E30 entries=56 hits=8 normalizedHits=0 misses=0           runtimeTranslations=0 runtimeGlslToMslCalls=0
```

`hit=8` = 3 个内建 program 的 6 个 stage + 自定义 shader 的 2 个 stage，全部 `AOT-cache`，
`misses=0`。这两个 key 的 MSL 与 `shaders.md` 里运行时落盘的 dump#6/#9 **字节相同**（§5.3）。
自定义 shader 与内建 program 走**同一条** `shaderSource` 钩子、同一张缓存。

### 6.2 画面抽查

Keybinds/Controls 子状态（保留上一轮 §52 的修复：字母统一蓝底白高光、左侧 LEFT/DOWN/UP/RIGHT
箭头各自颜色，且不闪烁）：`img/aot-keybinds-substate.png`
（LEFT 粉、DOWN 青、UP 绿、RIGHT 暗红；`D/F/J/K`、`U/I/A/S` 为白字蓝高光）。

音量托盘（FlxSoundTray，按 `+` 出现）：`img/aot-volume-tray.png`
（顶部中央 `VOLUME` 条正常显示，主菜单配色正常）。

两张截图都是本次 AOT 构建跑出来的，逐项与 `shaders.md` / README §52 描述一致。

---

## 7. 未命中时到底发生什么（"响亮"的具体定义）

```
[cne-aot] SHADER NOT AHEAD-OF-TIME COMPILED - no MSL cache entry
[cne-aot]   shader id : 999
[cne-aot]   stage     : vertex
[cne-aot]   cache key : 3C85C56EA5FFC9E4
[cne-aot]   cache file: 5C3E7B9A012E7E30 (56 entries)
[cne-aot]   source dump: /tmp/cne-aot-missing/3C85C56EA5FFC9E4.vert.glsl
[cne-aot]   regenerate with: ./metal/scripts/apply.sh --lime <lime>   (or: python3 metal/scripts/gen_shader_aot.py)
[cne-aot]   未 AOT，请重新生成缓存  (not AOT-baked: regenerate the shader cache; this shader is NOT translated at runtime)
[cne-aot]   --> if this source is genuinely composed at runtime (mod scripting), set
[cne-aot]       CNE_METAL_AOT_RUNTIME_TRANSLATE=1 to allow the legacy runtime translation.
[cne-aot]   GLSL source --------------------------------------------------
...完整源码...
```

同时：
* 该 stage 的原 GLSL 被交给原生层（原生按 MSL 塞进 `WrapShaderBin`）→ bgfx/Metal 报编译错误
  → `lime_gl_link_program` 失败 → OpenFL 打 "Unable to initialize the shader program"
  → 该 program 不出图。**不会静默变黑**：日志里既有我们的块，也有 bgfx 的编译错误。
* `misses` 计数 > 0，退出汇总额外可见。
* 注意：这里故意**不使用** `lime.utils.Log.error`（它 `throwErrors=true` 会抛异常），
  直接写 stderr —— 未命中不应该在渲染线程里抛异常。

---

## 8. 无法 AOT 的情形与策略

| 情形 | 能否 AOT | 出现概率 | 策略 |
| --- | --- | --- | --- |
| 引擎/库内建 shader（metadata） | ✅ | — | 全部进缓存（§3.2） |
| `assets/shaders/**`（`CustomShader`） | ✅ | — | 全部进缓存 |
| **构建时已存在的** mod `.frag/.vert` | ✅ | — | `mods/**` 会被枚举（§3.1 第 3 条） |
| **构建后才安装的** mod shader | ❌（构建期不知道） | 低 | 重新运行 `gen_shader_aot.py` 即可（mod 作者/打包者一次即可）；否则命中"响亮报错" |
| 运行时**由脚本拼接字符串**的 shader（`new FunkinShader(frag, vert)` 里 frag 来自脚本变量、或运行时改写 `shader.glFragmentSource`） | ❌（原理上不可能预知） | 极低：CNE 1.0.1 基础内容 0 处；mod 只要把 GLSL 放在 `assets/shaders/*.frag` 就属于可 AOT 的类别 | 命中"响亮报错"；需要时用显式 `CNE_METAL_AOT_RUNTIME_TRANSLATE=1` 打开旧行为（会打印警告 + 计入 `runtimeTranslations`，因此仍可审计） |
| 非 legacy GLSL（300 es / core） | ❌ | 低 | 翻译器本来就不支持；现在会以"未 AOT"报错（同样响亮） |

**结论**：本任务没有发现"必须保留运行时翻译"的类别；唯一真的无法 AOT 的是
**运行时动态拼接**的 GLSL，而基础内容与常规 mod（文件形式 GLSL）都不属于这一类。
因此默认行为设为"硬报错"，只留一个显式开关，符合"绝不静默回退运行时翻译"的要求。

---

## 9. 如何复现 / 排查

### 9.1 命令

```bash
cd CodenameEngine-1.0.1

# 重新生成缓存（apply.sh 已自动做；单跑也可）
python3 metal/scripts/gen_shader_aot.py

# 校验已提交的缓存是否与当前源码一致（CI 用；不一致退出码 4）
python3 metal/scripts/gen_shader_aot.py --check

# 构建（串行）
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832
./metal/scripts/check-compile.sh --lime ~/haxelib-dev/lime-832 --hxcpp ~/haxelib/hxcpp/4,3,2 --out /tmp/obj-check
haxelib run lime rebuild mac
haxelib run lime build mac
./metal/scripts/apply.sh --deploy --lime ~/haxelib-dev/lime-832   # 把 cpp ndll 装回 .app 并重签名
```

> `--deploy` 存在的原因：`lime build mac` 会把 `<lime>/ndll/MacArm64/lime.ndll`
> （**neko CLI 引导用的 ndll**，不含 `lime_metal_*` 符号）复制进 .app，覆盖掉
> `lime rebuild mac` 产出的 `lime-64.ndll`。不装回去的话 app 启动即 abort。

### 9.2 运行期开关

| 开关 | 作用 |
| --- | --- |
| `CNE_METAL_AOT_AUDIT=1` | 每个 shader 打印 `msl=AOT-cache key=…`；退出打印 `[cne-aot] EXIT …` |
| `CNE_METAL_SHADER_AUDIT=1` | 原生侧每 stage `origin=AOT-cache|runtime-translated`；program 汇总 |
| `CNE_METAL_AOT_RUNTIME_TRANSLATE=1` | **显式**允许运行时翻译（默认关闭；用于运行时拼 shader 的极端情形） |
| `CNE_METAL_DEBUG=1` | 落盘 MSL `/tmp/cne-shader-N.metal`（审计/对比用） |

### 9.3 `MSL → metallib` 也 AOT 了 —— 见 [`aot-coverage.md`](../../../../CodenameEngine-1.0.1/metal/audit/aot-coverage.md)

上一版这里写着"需要 `xcrun metal`（完整 Xcode），本次未做"。**该结论已被推翻**：
系统自带的 Metal 编译器（`newLibraryWithSource` 驱动的那个）可以在**构建期进程**里把 MSL
编成真正的 `.metallib`，再用公开的 `newLibraryWithData` 在运行时加载 ——
**本机不需要完整 Xcode**。bgfx v166 的 `ShaderMtl::create` 本来就支持 `"MTLB"` payload
（`renderer_mtl.cpp:4001`），所以 bgfx 零改动。

实现与实测全部记在 [`aot-coverage.md`](../../../../CodenameEngine-1.0.1/metal/audit/aot-coverage.md)：运行时 `newLibraryWithSource`
的引擎 shader 调用次数 = **0**（探针 swizzle 计数），每个 stage 的加载方式
（`metallib` / `source-compile`）都有 env-gated 日志。

---

## 10. 复现 / 证据文件

| 内容 | 路径 |
| --- | --- |
| 逐条缓存清单（生成） | `metal/audit/aot-coverage.md` |
| 全局/Controls 运行日志（AOT 证据 + 零回归） | `/tmp/aot-final-controls.log` |
| 截图运行的日志 | `/tmp/aot-controls-shot2.log`、`/tmp/aot-tray.log` |
| 进入歌曲的运行日志（含 `CustomShader` program 536） | `/tmp/aot-final-song.log` |
| 截图证据（Keybinds 子状态 / 音量托盘） | `metal/audit/img/aot-keybinds-substate.png`、`metal/audit/img/aot-volume-tray.png` |
| 审计时落盘的 10 份运行时 MSL（对比基准） | `/tmp/cne-shader-0..9.metal` |
| 前序审计（运行时链路/program 数据） | `metal/audit/shaders.md` |
| 生成器 | `metal/scripts/gen_shader_aot.py`、`metal/scripts/aot_shaders.py`、`metal/tools/GenShaderAot.hx` |
| 运行时 | `metal/haxe/lime/graphics/metal/ShaderAot.hx`、`ShaderAotCache.hx`、`MetalShaderBridge.hx` |
