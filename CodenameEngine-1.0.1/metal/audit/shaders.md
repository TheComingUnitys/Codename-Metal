# Shader 审计：所有 shader 是否真的经 Metal 渲染

> 审计对象：`CodenameEngine-1.0.1`（CNE 1.0.1）+ bgfx v166 Metal 后端
> （`metal/native`、`metal/haxe`、`~/haxelib-dev/lime-832`、`.haxelib/openfl/git`）
> 审计方式：**只读**（不改渲染行为）+ **env-gated 诊断**（默认关闭）。所有运行时结论都有日志/源码行号证据。
> 审计日期：2026-10-17。

---

## 0. 结论（TL;DR）

1. **是：运行时真实存在的每一个 program 都完整走了 Metal 管线** ——
   GLSL → `GLSLToMSL` → MSL → `WrapShaderBin`（26 字节自制容器）→ `bgfx::createShader`（Metal 后端 `newLibraryWithSource` 运行时编译）
   → `bgfx::createProgram` → 每笔 draw `bgfx::submit`。**没有任何一个 program 绕过这条链路，也没有任何"翻译失败→换默认 program"的兜底。**
2. 本次共实测到 **5 个 program**，全部通过：
   - 内建 3 个：`4`（OpenFL `DisplayObjectShader`）、`8`（`GraphicsShader`）、`14`（Flixel `FlxGraphicsShader`）——菜单/游戏内所有普通绘制；
   - 引擎自定义 2 个（会话 D，进歌后）：`162`（`CustomShader('engine/coloredVignette')`，由 `assets/data/stages/mall-evil.hx` 创建）、
     `536`（`CustomShader('pixelZoomShader')`，由 `assets/data/scripts/pixel.hx` 的 `makeCameraPixely()` 创建）。
   5/5 都：MSL 生成成功、bgfx program handle 有效、**声明的 attribute 100% 被 `glVertexAttrib*` 提供**、**所有 uniform 都有运行时写入**、每帧有真实 submit。
3. 唯一**未喂值**的 uniform 是 `_camSize`，且**只发生在内建 program 14（OpenFL `Shader` 路径）**：
   `writes=0`，同时全局 `lime_gl_uniform4f/uniform4fv` 在无自定义 shader 的会话里 = 0。
   **但两个 CustomShader program（162/536）的 `_camSize` 都被正常喂值**（`writes=1`，值 `(0,0,2560,1440)`）。
   → 影响判定：program 14 的 `main()` 不使用 `_camSize`（只用 tile 贴图），**内置渲染无影响**；
   真正使用 `textureCam` 系 helper 的自定义 shader 路径（program 162/536）**工作正常**。该差异在 OpenFL Haxe 层，非 Metal 后端引入。
4. **翻译/编译/链接失败、无 program 丢绘制** 在全部会话中均为 **0**：
   `shaderCompileFailures=0 / programLinkFailures=0 / drawsWithoutProgram=0 / uniformLocationMisses=0 / shadersNotMsl=0`。
5. 定义但**从未实例化**的 shader 类（没有 program、无运行时证据）：OpenFL mask shader ×2、
   `openfl.filters.*` 的 12 个 filter shader、flxanimate `FilterShader`、CNE `OverlayShader`/`WiggleShader`、
   flixel postprocess `Shader`（编译期被排除）。同管线，未触发（见 §8）。
6. 诊断工具 `CNE_METAL_SHOT` 在**当前构建里是坏的**：`HostCallback::screenShot` 只把像素存进 `gShotWrite`，
   全文件没有任何消费者把它写成 `.tga`（`grep gShotWrite metal/native/MetalContext.cpp` 只有写入点）。与本审计渲染结论无关。

**一句话回答任务标题**：所有**实际被创建并提交**的 shader 都真的经 Metal 渲染（5/5 详细验证，含自定义 shader），无降级、无旁路。
"缺口"只有：① 从未被实例化的 shader 类（无运行时证据）；② program 14 的 `_camSize` 不上传（OpenFL 层既有行为，对内置渲染无影响）；
③ 若 fragment 翻译失败而 vertex 成功，`bgfx` 会把单 shader 当 compute program 并误报链接成功（潜在隐患，未触发，见 §5c）。

---

## 1. 证据来源与复现方法

### 1.1 运行时诊断（本次新增，全部 env-gated、默认关闭、零渲染语义改动）

native 侧新增（`metal/native/MetalBindings.cpp`，详见附录 C）：

| 开关 | 输出 |
| --- | --- |
| `CNE_METAL_SHADER_AUDIT=1` | 每个 program 的 `[cne-shader-audit]` 块：`configure`（声明表）与 `first-submit`（运行时覆盖）、`COMPILE-FAILED` / `LINK-FAILED` / `NOT-MSL` / `DRAW-DROPPED` / `UNIFORM-LOCATION-MISS`、退出时 `EXIT` 汇总 |
| `CNE_METAL_SHADER_AUDIT_FRAME=<n>` | 首次 per-program 覆盖打印的帧号（默认 60） |
| `CNE_METAL_DEBUG=1`（原有） | 每个 shader 的 MSL 落盘 `/tmp/cne-shader-<n>.metal` + 26 字节容器头 hex + program attribute 表 |
| `CNE_METAL_DRAWLOG=1`（原有） | 关闭逐笔 draw 日志（只保留过滤窗口内的帧），避免日志爆炸 |
| `CNE_METAL_STATS=1`（原有） | 每 120 帧的 present/bgfxDraws/submitDraw 计数 |

`first-submit` 块格式：

```
[cne-shader-audit] program=14 phase=first-submit linked=1 bgfxProgram=12 vs=12 fs=13 submits=322
[cne-shader-audit]   msl program=14 vsDump=/tmp/cne-shader-4 fsDump=/tmp/cne-shader-5
[cne-shader-audit]   attrib program=14 name=openfl_Alpha glLoc=1 bgfxSlot=0 provided=1
...
[cne-shader-audit]   uniform program=14 name=_camSize sampler=0 members=1 comps=4 bgfxUniform=13 valid=1 writes=0 unit=0 value=0.0000,...
```

- `writes` = `lime_gl_uniform*` 真正写到该 uniform 的次数（`0` = 运行时从未喂值）。
- `provided` = 该 attribute 的 GL location 是否至少有一次被 `glVertexAttribPointer` / `glVertexAttrib*f` 提供过。
- `EXIT programs=…` 汇总包含**创建过但从未被 draw 的 program**（用于抓"死 program"）。

### 1.2 运行时会话

全部使用同一份含诊断的 ndll（`export/release/macos/bin/CodenameEngine.app/Contents/MacOS/lime.ndll`，
mtime 2026-10-17 09:48，`strings | grep cne-shader-audit` = 12；`bgfx renderer: Metal (Metal: yes)`）。

| # | 会话 | 帧数 | program | 说明 | 日志 |
| --- | --- | --- | --- | --- | --- |
| A | 标题→主菜单→Options→CONTROLS（自动按键） | 328 | 4, 8, 14 | 菜单路径 | `/tmp/as-audit-smoke.log` |
| B | 标题→主菜单→Freeplay→进歌（eggnog，无 shader） | 9001 | 4, 8, 14 | 游戏内、无自定义 shader | `/tmp/as-run-game3.log` |
| C | 历史长会话（task-2：标题/菜单/Options/子状态/故事→游戏） | 45459 笔 submit | 4, 8, 14 | 独立交叉验证 | `/tmp/log_FULL.log` |
| D | 标题→主菜单→Freeplay→进歌（winter horrorland 系内容，触发 CustomShader） | ~6361 | **4, 8, 14, 162, 536** | **自定义 shader 验证** | `/tmp/as-run-game4.log`（备份 `/tmp/shader-audit/sessionD-customshader.log`） |

会话 D 的 `EXIT` 汇总（节选）：

```
[cne-shader-audit] EXIT programs=5 shaderCompileFailures=0 programLinkFailures=0 drawsWithoutProgram=0 uniformLocationMisses=0 shadersNotMsl=0
[cne-shader-audit] EXIT program=4   ... submits=36361 uniforms=4  attribs=5
[cne-shader-audit] EXIT program=8   ... submits=6971  uniforms=4  attribs=5
[cne-shader-audit] EXIT program=14  ... submits=48155 uniforms=7  attribs=8
[cne-shader-audit] EXIT program=162 ... submits=3851  uniforms=10 attribs=8
[cne-shader-audit] EXIT program=536 ... submits=180   uniforms=8  attribs=8
```

> 注：program id 不是连续的（162/536）：`MetalContext::Register` 的 id 计数与整个会话创建过的 GL 对象数量相关，
> 游戏内大量纹理/缓冲创建会把 id 推高；这不影响句柄有效性（`bgfxProgram=13/14` 连续有效）。

---

## 2. 谁创建 program —— 全部创建点枚举

### 2.1 结论：只有 2 条活路径 + 1 条死路径

| # | 创建点 | 文件:行 | 谁调用 | 覆盖的 shader | 本次是否触发 |
| --- | --- | --- | --- | --- | --- |
| 1 | `Shader.__createGLShader` / `Shader.__createGLProgram` | `.haxelib/openfl/git/src/openfl/display/Shader.hx:320`、`:344`（`createShader` 324 / `shaderSource` 325 / `compileShader` 326 / `createProgram` 351 / `bindAttribLocation` 356-362 / `attachShader` 363-364 / `linkProgram` 365） | 所有 `openfl.display.Shader` 子类的**首次绘制**（`Shader.__init()` → `__initGL()`，`Shader.hx:466`；`__createGLProgram` 调用点 `:514`） | `DisplayObjectShader`、`GraphicsShader`、`FlxGraphicsShader`（Flixel `FlxShader` typedef）、`Context3DMaskShader`、`openfl.filters.*`、flxanimate `FilterShader` | ✅ program 4/8/14 |
| 2 | `FunkinShader.__createGLShader` / `__createGLProgram`（CNE 覆盖实现，**同样的 5 个 GL 调用**，但自己拼 `#version 120` 前缀并做错误日志解析） | `source/funkin/backend/shaders/FunkinShader.hx:81`、`:163`（`createShader` 85 / `compileShader` 87 / `createProgram` 173 / `linkProgram` 187；`#version ${glslVer}` 在 `:281`；`__createGLProgram` 调用点 `:312`） | CNE `FunkinShader` 及其子类：`CustomShader`（mod/资产 shader）、`OverlayShader`、`WiggleEffect.WiggleShader` | 引擎/资产自定义 shader | ✅ program **162**（coloredVignette）、**536**（pixelZoomShader） |
| 3 | `Program3D.__uploadShaders`（Stage3D / AGAL） | `.haxelib/openfl/git/src/openfl/display3D/Program3D.hx:853-920`（`createShader` 860/872，`linkProgram` 916）；AGAL 汇编入口 `AGALMiniAssembler.hx:178` → `Context3D.createProgram` | 真正的 Stage3D 3D 内容 | — | ❌ CNE 1.0.1 没有 Stage3D/AGAL 内容（`flx3d` 用普通 OpenFL 显示列表，不创建 `Program3D`）；这些类虽然被编译进二进制，但 2D 渲染路径不调用 |

**除了以上 3 处，仓库里没有任何其它 `glCreateShader` / `linkProgram` / `useProgram` / `glProgramSource` 调用点。**
逐目录搜索结论：

- `metal/native/**`：只**实现** Lime 的 `lime_gl_*` 原语（`lime_gl_create_shader` 等），不主动创建 shader。
- `metal/haxe/**`：`GLSLToMSL.hx` 只翻译源码；`MetalShaderBridge.hx` 只记台账。
- `~/haxelib-dev/lime-832`：`GL`/`NativeOpenGLRenderContext` 是对原生原语的转发；`lime/graphics/opengl/GLShader.hx:26` 的 `fromSource()` 便利函数**全仓库无调用者**。
- `.haxelib/flixel/git`：`flixel/effects/postprocess/Shader.hx` 被 `#if FLX_POST_PROCESS` 排除（`Options.txt` 无该 define，构建产物无 `postprocess/Shader.cpp`；`PostProcess.cpp` 编译成只打日志的 stub）。
- `openfl.filters.BitmapFilterShader` 的 12 个子类都只提供 GLSL 字符串，编译统一走路径 1。
- CNE `source/funkin/backend/shaders/*.hx` 全部继承 `FlxShader`/`Shader`，没有自己调 GL 的创建函数（`FunkinShader` 的 override 除外，即路径 2）。

### 2.2 Haxe → 原生 的 7 个桥接点（一个不漏）

`metal/scripts/patch_lime_haxe.py` 在 `NativeOpenGLRenderContext.hx` 里注入 7 个 `#if metal` 钩子（`~/haxelib-dev/lime-832` 检出的实际行号）：

| 钩子 | 行 | 作用 |
| --- | --- | --- |
| `createShader` | `:1086` | 记住 shader 是 vertex 还是 fragment |
| `shaderSource` | `:2800` | **GLSL → MSL 翻译**后交给原生 |
| `attachShader` | `:690` | 记 program→shader 归属 |
| `bindAttribLocation` | `:717` | 记 attribute 名→GL location |
| `linkProgram` | `:2661` | 链接后推 attribute/uniform 表给原生（`lime_metal_configure_program`） |
| `deleteShader` / `deleteProgram` | `:1203` / `:1166` | 清台账 |

原生实现（`metal/native/MetalBindings.cpp`）：

| 原语 | 行 | 行为 |
| --- | --- | --- |
| `lime_gl_create_shader` | 1583 | 分配 GL shader id + `ShaderData` |
| `lime_gl_shader_source` | 1594 | 保存 MSL 源码（Haxe 已翻译好） |
| `lime_gl_compile_shader` | 1616 | `WrapShaderBin` → `bgfx::createShader` |
| `lime_gl_create_program` / `attach_shader` | 1750 / 1759 | 分配 program，挂 shader |
| `lime_metal_configure_program` | 1877 | 建立 attribute slot 表 + 每个 uniform 的 `bgfx::createUniform` |
| `lime_gl_link_program` | 1980 | `bgfx::createProgram(vsh, fsh)` |
| `lime_gl_use_program` | 2044 | 只记录当前 program |
| `lime_gl_get_uniform_location` / `bind_attrib_location` / `get_attrib_location` | 见附录 C | 名称→location 查询 |

---

## 3. 渲染链路（每个 program 都走同一条）

```
OpenFL/Flixel/CNE 用 GLSL 字符串调 gl.createShader/shaderSource/compileShader
        │
        ├─ Hook NativeOpenGLRenderContext.createShader → MetalShaderBridge 记 stage
        ├─ Hook shaderSource → GLSLToMSL.translateSafe(GLSL) ─► MSL 源码
        │        （失败 → 返回原始 GLSL，见 §5）
        ├─ native lime_gl_shader_source 保存 MSL
        ├─ native lime_gl_compile_shader:
        │        WrapShaderBin(vertex?, MSL) → 26 字节 bgfx shader-bin 头 + MSL + NUL
        │        bgfx::createShader(blob) → Metal 后端 newLibraryWithSource 运行时编译 MSL
        ├─ attachShader / linkProgram:
        │        bgfx::createProgram(vsh, fsh)
        │        Hook linkProgram → MetalShaderBridge.onLinkProgram:
        │              attributeMap: glLocation → bgfx a_* slot
        │              uniformMap:  name / bgfx 名 / offset / memberCount / components / sampler / unit
        │              native lime_metal_configure_program → bgfx::createUniform（Vec4×N 或 Sampler）
        └─ 每笔 draw: BindVertexStreams（真实属性 + 常量属性物化流）+ ApplyUniformsBeforeDraw（setUniform/setTexture）
                     + bgfx::submit(view, programHandle)
```

容器头实测（`CNE_METAL_DEBUG=1`，program 4 vertex）：`5653480c00000000000000000000000000000000000019080000`

- 4 字节 magic：`56 53 48 0c` = `'V','S','H', version=12`（bgfx `Context::createShader` 拒绝 <12）
- 3×4 字节 hash / 2×4 字节 raw 掩码全 0；2 字节 constCount=0；4 字节 size=`0x0819`=2073 = mslBytes；随后 MSL + NUL。
  详见 `metal/native/MetalBindings.cpp:743` `WrapShaderBin`。

Metal 端接受源码的证据：`bgfx-master/src/renderer_mtl.cpp` `ShaderMtl::create` 里，
payload 不以 `"MTLB"` 开头时走 `newLibraryWithSource`（`renderer_mtl.cpp:4001-4007`），函数名固定 `xlatMtlMain`、
uniform 结构固定 `_mtl_u`（`SHADER_FUNCTION_NAME` / `SHADER_UNIFORM_NAME`）。**所以全部 5 个 program 的 MSL 都是 Metal 运行时编译的。**

---

## 4. 每个 program 一行（运行时实测）

| program id | 来源类 | GLSL 从哪来 | MSL 翻译 | bgfx program handle | attribute（provided/声明） | uniform（fed/声明） | 运行时 submit | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **4** | `openfl.display.DisplayObjectShader`（OpenFL 位图/文字/Sprite 显示列表通道） | `openfl/display/DisplayObjectShader.hx` 的 `@:glVertex/FragmentHeader/Body/Source`，经 `ShaderMacro` 展开 + `#ifdef GL_ES` 精度前缀 | ✅ `/tmp/cne-shader-0.metal`(2073B) / `-1`(2421B)，含 `xlatMtlMain` + `_mtl_u` + `s_openfl_Texture` | ✅ `bgfxProgram=10` | **5/5** | **4/4**（openfl_Matrix、openfl_HasColorTransform、openfl_TextureSize、openfl_Texture） | 1635（A）/ 10405（旧菜单长会话）/ 45360（B）/ 36361（D） | ✅ Metal |
| **8** | `openfl.display.GraphicsShader`（OpenFL Graphics/Shape 通道；CNE 用 `source/openfl/display/GraphicsShader.hx` 覆盖，内容与 haxelib 版一致） | 同上，fragment 用 `sampler2D bitmap` | ✅ `/tmp/cne-shader-2.metal`(2069B) / `-3`(2377B)，`_mtl_u` + `s_bitmap` | ✅ `bgfxProgram=11` | **5/5** | **4/4**（openfl_Matrix、openfl_HasColorTransform、openfl_TextureSize、bitmap） | 327（A）/ 9375（B）/ 6971（D） | ✅ Metal |
| **14** | `flixel.graphics.tile.FlxGraphicsShader`（Flixel 批量 tile 渲染：**所有 FlxSprite/FlxText/Alphabet/图标/音符/血条/背景**） | `.haxelib/flixel/git/flixel/graphics/tile/FlxGraphicsShader.hx`（`@:glVertexSource` + `@:glFragmentHeader` 含 helper 函数） | ✅ `/tmp/cne-shader-4.metal`(2412B) / `-5`(3533B)，`_mtl_u` 含 `openfl_Matrix/hasTransform/hasColorTransform/_camSize` | ✅ `bgfxProgram=12` | **8/8** | **6/7 有写入**（唯一未喂：`_camSize`，见 §6.3；对内置渲染无影响） | 1899（A）/ 163137（B）/ 48155（D） | ✅ Metal |
| **162** | **`funkin.backend.shaders.CustomShader` `'engine/coloredVignette'`**（CNE `FunkinShader` 子类；由 `assets/data/stages/mall-evil.hx` 创建） | `assets/shaders/coloredVignette.frag`（`#pragma header` → CNE `fragHeader` 展开 + 自定义 `color/amount/strength`）；vertex 用 CNE `ShaderTemplates.vertHeader`（8 attribute） | ✅ `/tmp/cne-shader-6.metal`(2407B) / `-7`(3994B)；`_mtl_u` 含 `_camSize/color/amount/strength`，`s_bitmap` | ✅ `bgfxProgram=13` | **8/8**（含 Flixel 内建 8 个） | **10/10**（6 内建 + color/amount/strength + bitmap） | 3851（D） | ✅ **Metal（自定义 shader 路径）** |
| **536** | **`funkin.backend.shaders.CustomShader` `'pixelZoomShader'`**（由 `assets/data/scripts/pixel.hx:137` `makeCameraPixely()` 创建） | `assets/shaders/pixelZoomShader.frag` + CNE 头 | ✅ `/tmp/cne-shader-8.metal`(2407B) / `-9`(3668B)；`_mtl_u` 含 `_camSize/pixelZoom`，`s_bitmap` | ✅ `bgfxProgram=14` | **8/8** | **8/8**（6 内建 + pixelZoom + bitmap） | 180（D） | ✅ **Metal（自定义 shader 路径）** |

### 4.1 program 4 / 8 的声明表（运行时 `configure` 块，节选）

```
[cne-shader-audit] program=4 phase=configure linked=1 bgfxProgram=10 vs=2 fs=3 submits=0
[cne-shader-audit]   msl program=4 vsDump=/tmp/cne-shader-0 fsDump=/tmp/cne-shader-1
[cne-shader-audit]   attrib program=4 name=openfl_Alpha glLoc=1 bgfxSlot=0 provided=0
...（provided 在 first-submit 块变为 1）
[cne-shader-audit]   uniform program=4 name=openfl_Matrix sampler=0 members=4 comps=4 bgfxUniform=6 valid=1 writes=0 unit=0 value=0.0000,...
```

program 8 结构相同，sampler 名为 `bitmap`。

### 4.2 program 14 的声明表（运行时）

```
program=14 linked=1 bgfxProgram=12 vs=12 fs=13
attrib  openfl_Position glLoc=0 bgfxSlot=3 ; openfl_Alpha glLoc=1 bgfxSlot=0
attrib  openfl_ColorMultiplier glLoc=2 bgfxSlot=1 ; openfl_ColorOffset glLoc=3 bgfxSlot=2
attrib  openfl_TextureCoord glLoc=4 bgfxSlot=4
attrib  alpha glLoc=5 bgfxSlot=5 ; colorMultiplier glLoc=6 bgfxSlot=6 ; colorOffset glLoc=7 bgfxSlot=7
uniform openfl_Matrix            members=4 comps=4  writes=322
uniform openfl_HasColorTransform members=1 comps=1  writes=322  value=0
uniform openfl_TextureSize       members=1 comps=2  writes=322
uniform hasColorTransform        members=1 comps=1  writes=322  value=0
uniform hasTransform             members=1 comps=1  writes=322  value=1
uniform _camSize                 members=1 comps=4  writes=0    ← 唯一未喂值（见 §6.3）
uniform bitmap                   sampler=1            writes=644  unit=0
```

### 4.3 program 162 / 536（CustomShader）的声明表（运行时，first-submit）

```
[cne-shader-audit] program=162 phase=first-submit linked=1 bgfxProgram=13 vs=160 fs=161 submits=1
[cne-shader-audit]   msl program=162 vsDump=/tmp/cne-shader-6 fsDump=/tmp/cne-shader-7
[cne-shader-audit]   attrib program=162 name=openfl_Position glLoc=0 bgfxSlot=3 provided=1
...（8 个 attribute 全 provided=1）
[cne-shader-audit]   uniform program=162 name=openfl_Matrix         writes=1 value=0.0008,0.0000,...,0.0014,0.0000
[cne-shader-audit]   uniform program=162 name=openfl_HasColorTransform writes=1 value=0
[cne-shader-audit]   uniform program=162 name=openfl_TextureSize    writes=1 value=2560.0000,1440.0000
[cne-shader-audit]   uniform program=162 name=hasColorTransform     writes=1 value=0
[cne-shader-audit]   uniform program=162 name=hasTransform          writes=1 value=0
[cne-shader-audit]   uniform program=162 name=_camSize              writes=1 value=0.0000,0.0000,2560.0000,1440.0000  ← 有喂值！
[cne-shader-audit]   uniform program=162 name=color                 writes=1 value=1.0000,0.0000,0.0000
[cne-shader-audit]   uniform program=162 name=amount                writes=1 value=1.0500
[cne-shader-audit]   uniform program=162 name=strength              writes=1 value=0.2500
[cne-shader-audit]   uniform program=162 name=bitmap sampler=1      writes=1 unit=0
```

program 536 同构：`_camSize writes=1 value=(0,0,2560,1440)`、`pixelZoom writes=1 value=0.1667`，8 attribute 全 provided=1。

### 4.4 MSL 证据（节选，program 4）

```metal
struct XlatMtlUniforms { float4x4 openfl_Matrix; float4 openfl_HasColorTransform; float4 openfl_TextureSize; };
struct XlatMtlVertexIn {
    float a_position [[attribute(0)]]; float4 a_normal [[attribute(1)]];
    float4 a_tangent [[attribute(2)]]; float4 a_bitangent [[attribute(3)]]; float2 a_color0 [[attribute(4)]]; };
vertex XlatMtlVaryings xlatMtlMain(const XlatMtlVertexIn xlatIn [[stage_in]],
        constant XlatMtlUniforms& _mtl_u [[buffer(0)]]) { ... }
fragment XlatMtlFragOut xlatMtlMain(XlatMtlVaryings xlatIn [[stage_in]],
        constant XlatMtlUniforms& _mtl_u [[buffer(0)]],
        texture2d<float> s_openfl_Texture [[texture(0)]], sampler s_openfl_TextureSampler [[sampler(0)]]) { ... }
```

program 14 fragment 的 `XlatMtlUniforms` 是
`{ float4 openfl_HasColorTransform; float4 openfl_TextureSize; float4 hasTransform; float4 hasColorTransform; float4 _camSize; }`
—— 正是 §4.2 的 5 个 fragment uniform；vertex 侧是
`{ float4x4 openfl_Matrix; float4 openfl_HasColorTransform; float4 openfl_TextureSize; float4 hasColorTransform; }`。
两 stage 的并集 = 7 个 uniform，与运行时表一致。

program 162 fragment 的 `XlatMtlUniforms`：
`{ openfl_HasColorTransform, openfl_TextureSize, hasTransform, hasColorTransform, _camSize, color, amount, strength }`
（+ VS 的 `openfl_Matrix`），与 §4.3 的 10 个 uniform 一致。program 162 的 MSL 里还能看到
`camToOg/getCamPos` 使用 `_camSize`，而运行时 `_camSize=(0,0,2560,1440)`（= 视口尺寸）——**该自定义 shader 的相机映射参数是正确的**。

---

## 5. 翻译失败 / 降级路径分析

**核心结论：没有"翻译失败就静默用别的 program"的兜底。所有失败都是"该 program 不出图 + 日志/计数器"，不会串到别的 program。**

| # | 路径 | 触发条件 | 实际行为 | 本次运行时证据 |
| --- | --- | --- | --- | --- |
| a | **GLSL→MSL 翻译失败（唯一"回退"点）** | `GLSLToMSL.translateSafe()` 抛异常或 `errors.length>0` | `MetalShaderBridge.onShaderSource` **返回原始 GLSL**（`MetalShaderBridge.hx:72-83`）；原生把它当 MSL 塞进 `WrapShaderBin`；bgfx/Metal 编译失败 → `shaderCompiled=false` → `lime_gl_link_program` 失败 → OpenFL 打 "Unable to initialize the shader program" → 该 program 的 draw 被丢弃 | 4 个会话全部：`shadersNotMsl=0`、`shaderCompileFailures=0`、`programLinkFailures=0`、`drawsWithoutProgram=0`。历史日志确有此类失败（2026-10-14 修复 sampler 参数之前的构建，`/tmp/bin-run.log` 17:40 "Unable to initialize the shader program"），**说明失败是响亮的、不会静默降级到别处** |
| b | 空 shader 源码 | `shader.source.empty()` | `compiled=false`，infoLog="empty shader source" | 0 次 |
| c | **FS 编译失败但 VS 成功**（潜在隐患） | a 只发生在 fragment stage | `lime_gl_link_program` 用 `bgfx::createProgram(vsh, BGFX_INVALID_HANDLE)` —— bgfx API 会把"单 shader"当成 **compute program**（`bgfx.cpp:6001-6006` → `createProgram(csh)`）并返回**有效** handle，于是我们 `linked=true`（会向 OpenFL 误报 LINK_STATUS=1）；首次 draw 时 Metal 后端以 NULL fragment function 建 PSO → `[cne-metal] bgfx fatal: Failed to create graphics PSO!`（`HostCallback::fatal` 只打印不 abort，`MetalContext.cpp:512`），该 program 不出图 | **未触发**（0 compile/link failure）。若将来出现，表现是"部分内容整体消失 + bgfx fatal 日志"，仍不会串到别的 program |
| d | 当前 program 无效 | `useProgram` 到未链接 program / `CurrentProgram()` 为空 | `SubmitDraw` 直接 return（新增 `DRAW-DROPPED` 计数/日志，前 10 次） | `drawsWithoutProgram=0` |
| e | program 缓存复用 | OpenFL `__context.__programs` 以 "vertex+fragment 源码" 为 key（`Shader.hx:504`） | 相同源码的多个 Shader 实例**共用**一个 GL program（例如 DisplayObjectShader 同时服务 Bitmap/TextField/Video） | 不是降级，但解释了为什么 3 个内建类只产生 3 个 program；不同源码（如两个 CustomShader）各得一个 program（162/536） |
| f | bgfx varying 接口校验被绕过 | `WrapShaderBin` 把 hashIn/hashOut 都写 0 | `bgfx_p.h:7509` 的 `vsr.m_hashOut != fsr.m_hashIn` 永不成立；varying 不匹配只能在 Metal PSO 创建时暴露（即 c 的 fatal 路径） | 未触发（5 个 program 全正常出图） |
| g | Haxe 台账缺失 | `attachShader` 钩子未触发 → `MetalShaderBridge.onLinkProgram` 提前 return | program 无 uniform 表 → 所有 `getUniformLocation` 返回 -1 → uniform 全部不喂 | 未触发：5 个 program 都有 `configure` 块，`uniformLocationMisses=0` |
| h | bgfx fatal 被降级为日志 | 任意 bgfx `BGFX_FATAL` | `HostCallback::fatal()` 只 `fprintf` 后 return（`MetalContext.cpp:512-515`），**不 abort**，bgfx 可能继续带 NULL 状态运行 | 4 个会话 `bgfx fatal` 出现 0 次；这是审计时排查"静默坏画面"的重要注意点 |

补充：`MetalShaderBridge.onShaderSource` 的"返回原 GLSL"是**唯一**的 source-level 回退，且回退后必然编译失败（不会变成可渲染的别的 program）。
`lime_gl_link_program` 里没有"找不到 fsh 就用默认 fsh"之类的逻辑；`SubmitDraw` 里也没有默认 program。

---

## 6. uniform 覆盖（声明集合 vs 运行时实际被 `lime_gl_uniform*` 喂过的集合）

### 6.1 机制（决定了"未喂值"的判定标准）

OpenFL 的 `Shader.__updateGL()`（`Shader.hx:750`）在每个 draw 前遍历 `__inputBitmapData` / `__paramBool` / `__paramFloat` / `__paramInt`，
对每个 `index >= 0` 的参数调用 `ShaderParameter.__updateGL()`：
- `value != null` → 写入真实值；
- `value == null` → 写入该类型的**零值**（如 `gl.uniform4f(index,0,0,0,0)`）。

所以：**`writes == 0` 严格等价于"该 uniform 的 Haxe `ShaderParameter.index < 0` 或根本不存在"**，而不是"值恰好为 0"。
（`index < 0` 时 `__updateGL` 首行 `if (index < 0) return;`，不会产生任何 GL 调用。）

sampler（`ShaderInput`）不同：纹理单元只在 `Shader.__enableGL()`（shader 切换为当前时）用 `gl.uniform1i` 写入，
所以 sampler 的 `writes` 次数天然远小于标量/向量 uniform（例如 program 4：`openfl_Matrix` 291 次 vs `openfl_Texture` 59 次）。

### 6.2 逐 program 差集

**program 4（DisplayObjectShader）— 差集为空**

| uniform | stage | GLSL 类型 | 运行时 writes（frame 60 首笔） | 结论 / 作用 |
| --- | --- | --- | --- | --- |
| `openfl_Matrix` | VS | mat4 | 291 | ✅ 每 draw 喂；顶点变换矩阵（缺 → 全部画到原点） |
| `openfl_HasColorTransform` | VS+FS | bool | 291（值 0） | ✅ 喂；颜色变换开关。为 0 = 正常状态；FlxSprite `.color` 等会置 1 并同时喂颜色常量属性 |
| `openfl_TextureSize` | VS+FS | vec2 | 291（如 67,65） | ✅ 喂；纹理尺寸（自定义 shader 用） |
| `openfl_Texture` | FS | sampler2D | 59（unit 0） | ✅ 喂；主纹理 |

**program 8（GraphicsShader）— 差集为空**：`openfl_Matrix` / `openfl_HasColorTransform` / `openfl_TextureSize` / `bitmap`，同上（sampler 名是 `bitmap`）。

**program 14（FlxGraphicsShader）— 差集 = { `_camSize` }**

| uniform | stage | GLSL 类型 | 运行时 writes | 结论 / 作用 |
| --- | --- | --- | --- | --- |
| `openfl_Matrix` | VS | mat4 | 322 | ✅ |
| `openfl_HasColorTransform` | VS+FS | bool | 322（0） | ✅ Flixel 的 `hasColorTransform` 独立变量同时喂（见下） |
| `openfl_TextureSize` | VS+FS | vec2 | 322 | ✅ |
| `hasTransform` | FS | bool | 322（**1**） | ✅ `FlxDrawQuadsItem.setParameterValue(shader.hasTransform, true)`（`.haxelib/flixel/git/flixel/graphics/tile/FlxDrawQuadsItem.hx:149`）；控制是否走 Flixel 颜色变换分支 |
| `hasColorTransform` | VS+FS | bool | 322（0） | ✅ `FlxDrawQuadsItem.hx:150` = `colored || hasColorOffsets`；本场景无着色精灵 → 0 正确 |
| **`_camSize`** | FS | vec4 | **0（未喂）** | ⚠️ 见 §6.3 |
| `bitmap` | FS | sampler2D | 644（unit 0） | ✅ |

**program 162（CustomShader coloredVignette）— 差集为空**

| uniform | writes（首笔） | 值 | 结论 / 作用 |
| --- | --- | --- | --- |
| `openfl_Matrix` / `openfl_HasColorTransform` / `openfl_TextureSize` / `hasTransform` / `hasColorTransform` | 1 | 矩阵 / 0 / 2560,1440 / 0 / 0 | ✅ 内建头 uniform，全部喂 |
| **`_camSize`** | **1** | **0,0,2560,1440** | ✅ **喂了**；`coloredVignette.frag` 经 `getCamPos()`/`textureCam()` 使用它，值 = 相机/视口尺寸 → 自定义 shader 的相机映射正确 |
| `color` | 1 | 1,0,0 | ✅ 脚本 `idk.color = [1,0,0]` |
| `amount` | 1 | 1.05 | ✅ 脚本 `postUpdate` 每帧写 `idk.amount` |
| `strength` | 1 | 0.25 | ✅ 同上 |
| `bitmap` | 1 | unit 0 | ✅ |

**program 536（CustomShader pixelZoomShader）— 差集为空**：`_camSize writes=1 value=(0,0,2560,1440)`、`pixelZoom writes=1 value=0.1667`，其余内建 uniform 同 162。

### 6.3 `_camSize` 的差异：program 14 未喂，CustomShader 正常 —— 影响判定

- **事实**：
  - program 14（OpenFL `Shader` 路径 / `FlxGraphicsShader`）：`_camSize` 声明存在、bgfx uniform valid，但 `writes=0`；
  - program 162 / 536（CNE `FunkinShader` 路径 / `CustomShader`）：`_camSize` `writes=1`，值就是相机/视口尺寸。
  - 交叉证据：不含自定义 shader 的会话（A/B/C）里 `lime_gl_uniform4f` / `lime_gl_uniform4fv` = 0（`/tmp/cne-coverage-*.txt`）；
    会话 D 出现 `_camSize` 写入（= `lime_gl_uniform4f` 被调用）→ 说明"vec4 uniform 从不上传"只适用于 program 14 这条路径。
- **作用**：`_camSize` 只被 Flixel `FlxGraphicsShader` 注入的 helper 使用：
  `getCamPos()` / `camToOg()` / `textureCam()`（`_camSize / vec4(openfl_TextureSize, openfl_TextureSize)`）。
  - program 14 自己的 `main()` **不调用**这些 helper（它只 `flixel_texture2D(bitmap, openfl_TextureCoordv)`）
    → **对内建 2D 渲染（所有 FlxSprite/Text/音符/背景）无影响**；
  - 真正用这些 helper 的是自定义 shader（coloredVignette/chromaticAberration 等），走 `FunkinShader` 路径，本会话实测 `_camSize` 正常喂值
    → **自定义 shader 的 `_camSize` 链路经 Metal 正常工作**，不是缺口。
- **归属**：program 14 的 `_camSize` 不上传发生在 OpenFL Haxe 层（`Shader.__processGLData` / `ShaderParameter.__updateGL` 的上传决策），
  **与 Metal 后端无关**；原始 OpenGL 路径执行同一段 Haxe 代码，因此同样不会上传。**不是本次 Metal 后端引入的回归**，
  影响范围仅限"把 `FlxGraphicsShader` 自身当作自定义 shader 用 `textureCam`"这种非典型用法。

### 6.4 任务点名要求的 uniform 逐一核对

| 名称 | 实际类别 | 结论 |
| --- | --- | --- |
| `openfl_HasColorTransform` | **uniform bool**（VS/FS 同名，shim 里按名字去重为一个 location） | ✅ 每 draw 喂（值 0/1），5 个 program 都覆盖 |
| `openfl_Alpha` | **attribute float**（不是 uniform） | ✅ 作为 vertex attribute 提供（见 §7）；OpenFL 用 `glVertexAttrib1f` 常量属性投递，后端把它物化成顶点流（`MetalBindings.cpp` `BindVertexStreams` 的 constant attribs 路径） |
| `openfl_ColorMultiplierv` / `openfl_ColorOffsetv` | **varying vec4**（VS 输出/FS 输入，不是 uniform） | ✅ 由 VS 写 `xlatOut`、FS 读 `xlatIn`；其输入 `openfl_ColorMultiplier/Offset` 是 attribute（见 §7）。颜色变换是否生效由 `openfl_HasColorTransform` 控制 |
| `openfl_TextureSize` | uniform vec2 | ✅ 5 个 program 都有写入 |
| `hasTransform` / `hasColorTransform`（Flixel FlxGraphicsShader） | uniform bool | ✅ program 14/162/536 每 draw 写入（值见 §6.2） |
| `_camSize`（Flixel helper 用） | uniform vec4 | ⚠️ program 14 未喂（无影响）；program 162/536 正常喂值 |
| `uTime` / `effectType` / `uSpeed` / `uFrequency` / `uWaveAmplitude`（`WiggleEffect.WiggleShader`） | 引擎自定义 shader 的 uniform | ⏸ 该 shader 类**本次运行未实例化**（只有 mod 脚本会 `new WiggleEffect`），无 runtime 数据；其创建走路径 2（与 162/536 同路径、已由 162/536 验证） |
| `uBlendColor`（`OverlayShader`） | 引擎自定义 shader uniform | ⏸ 同上：`OverlayShader` 在源码中无实例化点 |

---

## 7. attribute 覆盖

**结论：5 个 program 声明的每一个 attribute 在运行时都被提供过（`provided=1`），差集为空。**

| program | attribute 名 | GL location | bgfx slot（`a_*`） | 运行时是否提供 | 提供方式 |
| --- | --- | --- | --- | --- | --- |
| 4 / 8 / 14 / 162 / 536 | `openfl_Position` | 0 | 3 = `a_bitangent` | ✅ | `glVertexAttribPointer`（OpenFL 顶点缓冲） |
| 同上 | `openfl_Alpha` | 1 | 0 = `a_position` | ✅ | `glVertexAttrib1f` 常量属性 → 后端物化为顶点流 |
| 同上 | `openfl_ColorMultiplier` | 2 | 1 = `a_normal` | ✅ | 同上（vec4 常量属性） |
| 同上 | `openfl_ColorOffset` | 3 | 2 = `a_tangent` | ✅ | 同上 |
| 同上 | `openfl_TextureCoord` | 4 | 4 = `a_color0` | ✅ | `glVertexAttribPointer` |
| 14 / 162 / 536 | `alpha`（Flixel） | 5 | 5 = `a_color1` | ✅ | Flixel `ShaderBuffer` 每顶点数组（stride 4） |
| 同上 | `colorMultiplier`（Flixel） | 6 | 6 = `a_color2` | ✅ | 常量属性/每顶点数组（de-interleave 路径） |
| 同上 | `colorOffset`（Flixel） | 7 | 7 = `a_color3` | ✅ | 同上 |

证据：同一 session 的 `configure` 块 `provided=0` → `first-submit` 块 `provided=1`（`/tmp/as-audit-smoke.log` 行 2487-2517；
会话 D 的 program 162/536 同样 `configure provided=0` → `first-submit provided=1`）。
attribute slot 映射证据：`MetalBindings.cpp` `lime_metal_configure_program` 的
`[cne-dbg] program N attributes: glLoc=…->bgfxAttrib=…` 日志；
glLoc↔slot 对齐靠 `MetalShaderBridge.onLinkProgram` 的 attributeMap（`glBindAttribLocation` 合成 location 后回推）。

补充说明：
- `lime_gl_vertex_attrib1f/4f` 在日志里会打 `UNIMPLEMENTED primitive called`（`ReportUnimplemented`），
  但那是**过时误报**：常量属性已由 `CollectConstantAttribs` + `BindVertexStreams` 物化成真实顶点流（本表 `provided=1` 即证），
  且 `metal/audit/primitives.md` 的原语审计也确认这是误报，不是丢画面缺口。
- 每个 draw 的 `[cne-dbg]` 行会列出该笔实际提供的 `glLoc`（`layout: glLoc=0->attrib#0 …` 与 `constant attribs -> stream N … #0=…`），
  这是"声明 vs 提供"的逐笔证据。

---

## 8. 定义但未被实例化的 shader 类（覆盖缺口清单）

这些类**存在 GLSL 声明**，但本次运行（及进程内全部会话）里**没有被编译成 program**。
它们与 §4 的 5 个 program 共用 §2/§3 的同一条管线，一旦被使用也会走 Metal；但**没有运行时证据**，因此只写"同管线、未触发"。

| shader 类 | 文件 | 声明（attribute / uniform） | 为什么没有 program | 潜在触发场景 |
| --- | --- | --- | --- | --- |
| `Context3DMaskShader` | `.haxelib/openfl/git/src/openfl/display/_internal/Context3DMaskShader.hx:11` | 2 attr / `openfl_Matrix` + `openfl_Texture` | 该 OpenFL 版本里 `OpenGLRenderer` 只 `new` 了它赋给 `__maskShader`，**全仓库没有任何使用点**（遮罩改用 stencil + 默认 shader） | —（死代码） |
| `Context3DAlphaMaskShader` | 同上 `:11` | 同上 | 同上 | —（死代码） |
| `openfl.filters.*`（12 个：`BevelShader`/`ColorMatrixShader`/`HideShader`/`DisplacementMapShader`/`BlurShader`/`InvertAlphaShader`/`BlurAlphaShader`/`CombineShader`/`InnerCombineShader`/`CombineKnockoutShader`/`InnerCombineKnockoutShader`/`ConvolutionShader`） | `.haxelib/openfl/git/src/openfl/filters/{BevelFilter,ColorMatrixFilter,DropShadowFilter,DisplacementMapFilter,BlurFilter,GlowFilter,ConvolutionFilter}.hx` | 2 attr（openfl_Position/TextureCoord）+ 各自 uniform（如 `uRadius`/`uColor`/`uMultipliers`/`uConvoMatrix`…） | 仅当 `displayObject.filters = [new ShaderFilter(...)]` 时才首次绘制/编译；CNE 普通内容（标题/菜单/游戏）不用 filters 做视觉效果 | 编辑器 UI（`UISubstateWindow`/`UIWarningSubstate` 的 blur ShaderFilter） |
| flxanimate `FilterShader` | `.haxelib/flxanimate/git/flxanimate/Filters.hx:72` | 8 attr（继承 FlxShader）/ `u_time`、`HSV`、`matrices[99]` + 内建 uniform | FLA 动画滤镜；基础素材未使用 | 带滤镜的 flxanimate 动画 |
| CNE `OverlayShader` | `source/funkin/backend/shaders/OverlayShader.hx:6` | 8 attr / `uBlendColor` | **源码中无 `new OverlayShader`** | 目前无入口（疑似遗留/mods 用） |
| CNE `WiggleEffect.WiggleShader` | `source/funkin/backend/shaders/WiggleEffect.hx:65` | 8 attr / `uTime`、`effectType`、`uSpeed`、`uFrequency`、`uWaveAmplitude` | 只有 mod/脚本 `new WiggleEffect()` 才会创建 | mod 脚本 |
| flixel postprocess `Shader` | `.haxelib/flixel/git/flixel/effects/postprocess/Shader.hx` | — | 编译期 `#if FLX_POST_PROCESS` 未定义（`Options.txt` 无），构建产物无该类；`PostProcess.cpp` 是只打日志的 stub | —（不可用） |

> **CNE `CustomShader` 已在会话 D 实际验证**（program 162/536），不属于缺口。其余的引擎自定义 shader
> （`OverlayShader`/`WiggleShader`/`ScriptableShader` 动态 shader）都走**已由 162/536 验证过的同一条 `FunkinShader` 路径**，
> 但本次没有被图标/歌曲内容触发，故只能在文件里标注"未触发"。

---

## 9. 最终结论

**"是否所有 shader 都真的经 Metal 渲染？"**

- **对于运行时真正存在并提交的 shader program：是 —— 5/5（program 4 / 8 / 14 / 162 / 536）逐项验证通过。**
  - 全部由 `GLSLToMSL` 翻译成 MSL 并落盘（10 个 MSL 文件，均含 `xlatMtlMain`/`_mtl_u`/`s_<sampler>`）；
  - 全部装进 `WrapShaderBin` 的 26 字节 bgfx 容器并通过 `bgfx::createShader` 在 Metal 后端运行时编译；
  - 全部拿到有效 `bgfx::ProgramHandle` 并每帧 `bgfx::submit`（会话 D：36361/6971/48155/3851/180 笔）；
  - attribute 覆盖 5/5、5/5、8/8、8/8、8/8 全部 `provided=1`；
  - uniform 覆盖 4/4、4/4、6/7、10/10、8/8（唯一例外 program 14 的 `_camSize`，见 §6.3；两个真正使用它的自定义 shader 反而正常）。
- **不存在**"翻译失败 → 静默用默认/别的 program"的兜底；失败表现为"该 program 不出图 + 日志/计数器"，本次运行 0 次触发。
- **缺口（明确列出）**：
  1. program 14（OpenFL `Shader` 路径）的 `_camSize` 未被上传 —— OpenFL Haxe 层既有行为，非 Metal 回归；
     对内置渲染无影响，真正使用它的 CustomShader 路径正常。
  2. §8 的 shader 类（mask/filter/flxanimate/Overlay/Wiggle）**没有运行时证据**——同管线但未触发；
     其中 `Context3DMaskShader`/`Context3DAlphaMaskShader` 在这个 OpenFL 版本是死代码。
  3. 潜在隐患：若 fragment stage 翻译失败而 vertex 成功，`bgfx::createProgram(vsh, invalid)` 会退化成 compute program 并误报链接成功，
     首次 draw 触发 `bgfx fatal`（只打日志不 abort）。本次未触发，建议后续把这条路径显式判为 link 失败（改动 3 行，见 §11 建议）。
  4. 诊断工具 `CNE_METAL_SHOT` 当前不会写文件（见 §0.6），与渲染无关。

---

## 10. 复现 / 证据文件索引

| 内容 | 路径 |
| --- | --- |
| 会话 A 日志（标题/菜单/Options/CONTROLS） | `/tmp/as-audit-smoke.log` |
| 会话 B 日志（标题/菜单/Freeplay/进歌 eggnog，无自定义 shader） | `/tmp/as-run-game3.log` |
| 会话 C 日志（task-2 长会话，含故事→游戏） | `/tmp/log_FULL.log` |
| **会话 D 日志（触发 CustomShader：program 162/536）** | `/tmp/as-run-game4.log`（备份 `/tmp/shader-audit/sessionD-customshader.log`） |
| MSL 落盘（0-9，对应 5 个 program 的 10 个 stage） | `/tmp/cne-shader-0.metal` … `/tmp/cne-shader-9.metal`（备份 `/tmp/shader-audit/msl-sessionD/`） |
| 原语调用计数（audit-primitives 产出，用于交叉验证 uniform4f 计数） | `/tmp/cne-coverage-*.txt` |
| 解析脚本（本审计用） | `/tmp/shader-audit/parse.py` |

复现命令：

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1/export/release/macos/bin
CNE_METAL_DEBUG=1 CNE_METAL_DRAWLOG=1 CNE_METAL_SHADER_AUDIT=1 CNE_METAL_STATS=1 \
  ./CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/audit.log 2>&1
# 自动按键（帧:SDL扫描码；40=Return 81=Down 82=Up 41=Esc）：
#   CNE_METAL_AUTOKEYS="250:40,700:40,1300:81,1700:40,1900:81,2200:40"
#   标题→主菜单→Freeplay→进歌（进歌后会创建 CustomShader program）
grep -E 'cne-shader-audit|cne-dbg\] shader' /tmp/audit.log
python3 /tmp/shader-audit/parse.py /tmp/audit.log
```

---

## 11. 可选改进建议（未实施，仅记录）

1. **把 fragment 缺失判为链接失败**：`metal/native/MetalBindings.cpp:1980` `lime_gl_link_program` 中，
   若 `fs` 不存在或未编译，当前会走 `bgfx::createProgram(vsh, BGFX_INVALID_HANDLE)`（bgfx 当 compute program 处理并返回有效 handle）。
   建议改为 `data.linked=false; return;`，避免"误报 LINK_STATUS=1 + 首次 draw 触发 bgfx fatal"。3 行改动，不影响正常渲染。
2. **`_camSize` 排查**（若要修 OpenFL/Flixel 侧）：确认 OpenFL `Shader.__processGLData` 为何没有为 `FlxGraphicsShader` 的 `_camSize` 建立上传参数
   （可在 `ShaderParameter.__updateGL` 前打点或对比 GL 后端行为）；这是 Flixel/OpenFL 层问题，不属 Metal 后端，
   且 CNE 的 `FunkinShader` 路径没有该问题。
3. **`CNE_METAL_SHOT` 补上 `.tga` 写盘**（`HostCallback::screenShot` 已收像素，缺消费者）。

---

## 附录 A：会话 A 的 3 个内建 program 完整审计块

见 `/tmp/as-audit-smoke.log` 行 1-60（configure）与 2487-2545（first-submit）。逐条摘录见 §4、§6、§7。

## 附录 B：MSL 文件对应（会话 D 的 10 个 stage）

| MSL 文件 | 字节 | 类型 | 所属 program |
| --- | --- | --- | --- |
| `/tmp/cne-shader-0.metal` | 2073 | vertex | 4 (DisplayObjectShader) |
| `/tmp/cne-shader-1.metal` | 2421 | fragment | 4 |
| `/tmp/cne-shader-2.metal` | 2069 | vertex | 8 (GraphicsShader) |
| `/tmp/cne-shader-3.metal` | 2377 | fragment | 8 |
| `/tmp/cne-shader-4.metal` | 2412 | vertex | 14 (FlxGraphicsShader) |
| `/tmp/cne-shader-5.metal` | 3533 | fragment | 14 |
| `/tmp/cne-shader-6.metal` | 2407 | vertex | 162 (CustomShader coloredVignette) |
| `/tmp/cne-shader-7.metal` | 3994 | fragment | 162 |
| `/tmp/cne-shader-8.metal` | 2407 | vertex | 536 (CustomShader pixelZoomShader) |
| `/tmp/cne-shader-9.metal` | 3668 | fragment | 536 |

## 附录 C：本次新增诊断的代码位置（默认关闭，不改渲染行为）

`metal/native/MetalBindings.cpp`：

- `ShaderAuditEnabled()` / `ShaderAuditFrame()` / `ShaderAuditDumpProgram()` / `ShaderAuditDumpAllPrograms()`（helpers 区，`CNE_METAL_SHADER_AUDIT`）
- `ProgramData::Uniform::writeCount`、`ProgramData::{submitCount,fedAttribs,auditDumped}`、`ShaderData::auditDumpIndex`
- `lime_gl_compile_shader`：NOT-MSL 检测、COMPILE-FAILED 计数
- `lime_gl_link_program`：LINK-FAILED 计数
- `lime_metal_configure_program`：configure 块（声明表）
- `SubmitDraw`：DRAW-DROPPED 计数、per-program submit 计数、first-submit 覆盖块、退出汇总注册
- `WriteUniform` / `WriteMatrixUniform`（`MetalUniforms.inc`）/ `lime_gl_uniform_matrix3fv` / `lime_gl_uniform_matrix4fv`：`writeCount++`
- `BindVertexStreams`：`fedAttribs` 记录（真实属性 + 常量属性）
- `lime_gl_get_uniform_location`：UNIFORM-LOCATION-MISS 计数

## 附录 D：跨日志 program id 集合

- 2026-10-14 → 2026-10-17 会话 D 之前的所有 `/tmp/*.log`（约 60 份）：
  `[cne-attrib] getAttribLocation(program=N, …)` / `[cne-sub] prog=N` 只出现 **{4, 8, 14}**（非此集合出现次数 = 0）。
  代表日志：`/tmp/log_FULL.log`（45459 笔 submit，含故事→游戏）、`/tmp/ath_controls*.log`。
- **会话 D 首次出现 {162, 536} 两个 CustomShader program**（进歌内容不同）：program id 与实际 GL 对象创建数量相关，不连续；
  `EXIT programs=5` 即全部。
- `lime_gl_uniform4f` / `uniform4fv`：无自定义 shader 的会话（`/tmp/cne-coverage-*.txt`）为 **0**；
  会话 D 因 `_camSize` 被喂值而 >0。这与 §6.3 的差异结论一致。
