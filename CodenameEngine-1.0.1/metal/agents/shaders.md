# task-7 — GLSL→MSL 翻译的 `gl_Position` / NDC 方向排查报告

**作者**：agent-shaders ｜ **写入范围**：`metal/haxe/lime/graphics/metal/GLSLToMSL.hx`、`metal/agents/shaders.md`
**日期**：本轮排查 ｜ **状态**：完成（含 1 个已知遗留限制）

---

## 0. 结论速览（TL;DR）

| 问题 | 结论 | 依据 |
| --- | --- | --- |
| **bgfx 的 Metal 后端是否对 `gl_Position` 做 Y 翻转？** | **不做，也不该做** | `renderer_mtl.cpp` 全文没有任何 `gl_Position` 重写；viewport 为正高度、`originY = rect.y`（`renderer_mtl.cpp:6699-6706`）；bgfx 的 Vulkan 后端才是唯一用**负 viewport 高度**手动补偿 Y 的后端（`renderer_vk.cpp:11481-11488`、`3326-3333`） |
| **我们要不要在翻译器里对 `gl_Position.y` 取反？** | **绝对不要** | Metal 的 NDC 与 OpenGL 一样是 **+Y 向上**（origin 左下），只有 framebuffer/纹理坐标是左上原点。取反会让整个游戏上下颠倒 |
| **要不要处理 Z 到 `[0,1]`？** | **要，已实现** | Metal 的 clip space 深度是 `[0,w]`，OpenGL 是 `[-w,w]`；bgfx 对 Metal 报 `homogeneousDepth == false`，但 OpenFL 传进来的矩阵是 GL 风格的 `createOrtho(..., -1000, 1000)` |
| **`gl_Position` 是否被翻译对？** | 是 | `xlatOut.position` + `[[position]]`，无符号反转；见 §2 |
| **varying ↔ MSL 结构体对应？** | 正确 | 顶点/片元两侧同名同类型，`float4 position [[position]]` + 同名 varying |
| **`[[attribute(N)]]` 编号是否与宿主一致？** | 一致 | 见 §2.4（含实测 `glLoc=0 -> a_bitangent` 的解释） |
| **本次排查顺带发现的真 bug** | 12 个真实引擎 `.frag` 里 **12/12 都无法被 Metal 编译**；已修好 11 个 | §4 |

**对"几何体完全不可见"的判断**：翻译器**不是**全屏不可见的根因 —— OpenFL 的
`DisplayObjectShader` 与 CNE 的默认 `FunkinShader` 程序都能编译且能完成 pipeline link
（§5 已实测）。但它们之前会让**任何使用 CNE `.frag` 自定义着色器的对象必然不可见**
（bgfx `createShader` 失败），这一点值得与 uniform / state 那两条线并行核对
（`bgfxDraws=10` vs `glDrawElements≈33682` 的落差更像状态机/uniform 上传问题）。

---

## 1. NDC 方向：源码级证据链

### 1.1 bgfx 的 Metal 后端没有任何 `gl_Position` 翻转

- `bgfx-master/src/renderer_mtl.cpp:1212-1218` —— bgfx **手写**的 Metal 全屏三角形（screenshot blit）：
  ```
  v_id==0 -> gl_Position = (-1,-1), v_texcoord0 = (0,1)
  v_id==1 -> gl_Position = ( 3,-1), v_texcoord0 = (2,1)
  v_id==2 -> gl_Position = (-1, 3), v_texcoord0 = (0,-1)
  ```
  插值后 `v = (1 - y_ndc)/2`，即 **`y_ndc = +1` 对应纹理 `v = 0`**。Metal 纹理第 0 行是
  **图像顶部**，截图要正确不上下翻，就必须让 `y_ndc = +1` 落在画面上方 ⇒ Metal 的
  NDC 是 **+Y 向上**（与 OpenGL 视觉一致）。这也是 bgfx 对 Metal 唯一"手写"顶点着色器。
- `renderer_mtl.cpp:6699-6706` —— 渲染时设置的是**正高度** viewport：
  `originY = rect.m_y; height = rect.m_height; znear = 0; zfar = 1;`（无负高度、无 Y 取反）。
- `renderer_mtl.cpp:4160-4200` —— 属性按 **名字** 反射（`a_position`…`a_bitangent`），
  `[[attribute(n)]]` 的 n 直接当 Metal vertex attribute index 用。
- `tools/shaderc/shaderc_metal.cpp:674-687` —— bgfx 自己的 GLSL→MSL 编译器只设置
  platform / MSL version，**既没有设 `flip_vert_y` 也没有设 `fixup_clipspace`**；
  spirv-cross 的两个选项默认都是 false（`3rdparty/spirv-cross/spirv_glsl.hpp:180-186`）。
  也就是说：bgfx 认为"着色器输出的就是最终 NDC"，任何后端差异在**后端内部**解决。

### 1.2 唯一需要 Y 补偿的后端是 Vulkan，且它在后端内部解决

- `bgfx-master/src/renderer_vk.cpp:11481-11488`：
  ```cpp
  vp.y      = float(rect.m_y + rect.m_height);
  vp.height = -float(rect.m_height);      // ← 负高度 = 翻转 Y
  ```
  另一处 `3326-3333` 同样。Vulkan 的 NDC 才是 Y 向下。bgfx 对 D3D11（`renderer_d3d11.cpp:2296-2303`，
  正高度、`TopLeftY=0`）和 Metal 都**不翻转**。
- `bx/include/bx/math.h:1906-1955` —— `bx::mtxProj/mtxOrtho` 的参数里**没有 Y 翻转开关**，
  只有 `_homogeneousNdc`（深度 `[-1,1]` vs `[0,1]`）与 `_handedness`（Z 方向）。
- `bgfx-master/examples/05-instancing/instancing.cpp:212`、`examples/45-bokeh/bokeh.cpp:302`：
  `bx::mtxProj(proj, ..., bgfx::getCaps()->homogeneousDepth)` —— **同一套 Y**，只有深度标志分后端。
- `examples/45-bokeh/bokeh.cpp:446`：`screenSpaceQuad(caps->originBottomLeft)` ——
  `originBottomLeft` 只用于**全屏四边形取纹理的 UV**，与顶点 NDC 的 Y 无关。
  `renderer_gl.cpp:2936-2941` 显示它只在 GL 上被置 true（GL 的 framebuffer 原点在左下）。

结论：`bgfx::getCaps().originBottomLeft == false`（Metal）**不代表 NDC 的 Y 朝下**，
它描述的是 framebuffer/纹理原点。把它当成"要翻 Y"的依据是误读。

### 1.3 宿主侧矩阵：GL 风格，Y 已经被 OpenFL 自己翻好

`CodenameEngine-1.0.1/.haxelib/openfl/9,5,2/src/openfl/display/OpenGLRenderer.hx:976-977`：
```haxe
__projection.createOrtho(0, W, 0, H, -1000, 1000);          // 未翻转
__projectionFlipped.createOrtho(0, W, H, 0, -1000, 1000);   // __flipped 时使用
```
`lime/math/Matrix4.hx:355-380` 的 `createOrtho` 给出 `y_ndc = 2*y_view/H - 1`（未翻转）或
`y_ndc = 1 - 2*y_view/H`（翻转）。OpenFL 的 `y_view = 0` 是**屏幕顶**；走 `__projectionFlipped` 时
屏幕顶 → `y_ndc = +1`。Metal 与 OpenGL 都把 `y_ndc = +1` 画在屏幕顶部 ⇒ **语义完全一致，无需翻转**。

### 1.4 深度：这里才是真的不一致（已在翻译器修好）

- Metal viewport `znear=0, zfar=1`（`renderer_mtl.cpp:6706-6725`），bgfx 对 Metal 报
  `homogeneousDepth = false`（默认值，`bgfx.cpp:2228`），即 bgfx 的"契约"是着色器输出 `z ∈ [0,w]`。
- 但 OpenFL 的矩阵是 GL 风格：`z_ndc = -0.001 * z_view`（`createOrtho(..., -1000, 1000)`）。
  2D 内容 `z_view = 0 ⇒ z_ndc = 0`，恰好合法（所以不是本次全黑的原因），
  但任何 `z_view > 0` 的顶点在 Metal 下会被近平面裁掉，`z_ndc ∈ [-1,0)` 的内容也会被错误压缩。
- 修法等价于 spirv-cross 的 `fixup_clipspace`（`spirv_glsl.hpp:175-178` 注释：
  "MSL: rewrite `[-w, w]` depth (GL style) to `[0, w]` depth"）：在 `main` 末尾补
  `xlatOut.position.z = (xlatOut.position.z + xlatOut.position.w) * 0.5;`，只在顶点阶段且
  真的写了 `gl_Position` 时才补（`clipSpaceFixup()`）。对 `z_ndc = 0` 的内容行为不变，
  对有序深度是单调映射，不会破坏深度排序。

> 外部旁证：Metal NDC 原点在左下、+Y 向上，而 framebuffer/纹理坐标原点在左上、+Y 向下
> （[How to Understand Metal's Coordinate Systems](https://codingbestpractice.com/howto/how-to-understand-metals-coordinate-systems/)）。
> 与上面的 bgfx 源码证据一致。

---

## 2. `gl_Position` / varying / 片元输出 / 属性编号 逐项核对

以真实 OpenFL `DisplayObjectShader`（`openfl/display/DisplayObjectShader.hx:11-42`）为例，
翻译输出（`haxe -cp metal/haxe --run lime.graphics.metal.TestTranslate` 可复现）：

```metal
struct XlatMtlVaryings { float4 position [[position]]; float openfl_Alphav; ... };
struct XlatMtlVertexIn {
    float  a_position  [[attribute(0)]];   // openfl_Alpha
    float4 a_normal    [[attribute(1)]];   // openfl_ColorMultiplier
    float4 a_tangent   [[attribute(2)]];   // openfl_ColorOffset
    float4 a_bitangent [[attribute(3)]];   // openfl_Position
    float2 a_color0    [[attribute(4)]];   // openfl_TextureCoord
};
vertex XlatMtlVaryings xlatMtlMain(const XlatMtlVertexIn xlatIn [[stage_in]],
                                   constant XlatMtlUniforms& _mtl_u [[buffer(0)]]) {
    XlatMtlVaryings xlatOut;
    xlatOut.openfl_Alphav = xlatIn.a_position;
    xlatOut.openfl_TextureCoordv = xlatIn.a_color0;
    ...
    xlatOut.position = _mtl_u.openfl_Matrix * xlatIn.a_bitangent;
    xlatOut.position.z = (xlatOut.position.z + xlatOut.position.w) * 0.5;  // 本次新增
    return xlatOut;
}
```

1. **`gl_Position` 翻译正确**：→ `xlatOut.position`，结构体成员带 `[[position]]`，无符号改写。
   `gl_Position` 只作为写目标时行为符合 GLSL；`mat4 * vec4` 的列主序语义 GLSL/MSL 相同。
2. **varying 对应完整**：顶点 out / 片元 in 用同一个 `XlatMtlVaryings`（同名同类型成员 +
   `position`），MSL 按名字匹配；实测能通过 `MTLRenderPipelineState` link（§5）。
3. **片元输出正确**：`gl_FragColor` → `xlatOut.color`，`struct XlatMtlFragOut { float4 color [[color(0)]]; }`，
   `discard` → `discard_fragment()`。
4. **`[[attribute(N)]]` 与宿主一致**：翻译器按 GLSL **声明顺序**编号，宿主
   `MetalBindings.cpp` 的 `BgfxAttribForLocation()` 用 `lime_metal_configure_program` 给的
   `glLoc → 声明序` 映射来建 `bgfx::VertexLayout`，两者对上。
   实测的 `glLoc=0 → a_bitangent` 就是这么来的：`DisplayObjectShader` 的顶点头里
   `openfl_Alpha, openfl_ColorMultiplier, openfl_ColorOffset` 三个属性排在
   `openfl_Position` 前面，所以 `openfl_Position` 是第 4 个（索引 3 = `a_bitangent`），
   而 OpenFL 把它 bind 到 GL location 0。**这不是 bug**，只要宿主继续用同一张映射表即可。
   （CNE 自定义着色器的 `vertHeader` 同样有 4 个前置属性，索引规则一致。）

---

## 3. 本次对 `GLSLToMSL.hx` 的改动（唯一写入文件）

| # | 改动 | 原因 |
| --- | --- | --- |
| 1 | `run()` 里新增 `renameShadowedParameters(toks)`；新增 `renameShadowedParameters / renameShadowedLocals / renameIdentifiers / renameCommaDeclarators / parameterNames / paramNameOf` 及 `nextSignificant / previousSignificant / matchDelim / matchDelimBack` | **参数/局部变量遮蔽全局 uniform 时被错误替换**。`fragHeader` 里 `vec4 applyFlixelEffects(vec4 color)`，而 `coloredVignette.frag` 又有 `uniform vec3 color;` ⇒ 以前生成 `float4 _mtl_u.color.xyz`（语法/语义都错）。现在参数在 token 流里改名为 `_xlat_arg_color`，作用域内引用一起改，函数外不受影响 |
| 2 | `emit()` 增加 brace/paren 深度跟踪，对**文件作用域**声明补 `constant `（`const float x = 1.0;` → `constant float x = 1.0;`，`vec4 colorOutline = ...;` → `constant float4 ...;`） | MSL 要求 program scope 变量在 `constant` 地址空间，否则 "program scope variable must reside in constant address space"（`charterGrid.frag`、`editorBlur*.frag`、`editorWaveforms*.frag`） |
| 3 | `translateFunctionBody()` 把 `main` 里的裸 `return;` 改写成 `return xlatOut;` | GLSL 的 `return;` 在 `main` 表示提前结束；MSL 的 `main` 必须返回输出结构体（`charterGrid.frag`、`editorWaveforms.frag`） |
| 4 | `assemble()` 末尾按 `clipSpaceFixup()` 追加深度重映射（§1.4） | GL `[-w,w]` → Metal `[0,w]` |
| 5 | `injectHelperScope()` 扩展：除 `_mtl_u` / `xlatIn` 外，还按需为 helper 注入 **`thread <OutStruct>& xlatOut`**（按引用）与**全局 sampler**（`texture2d<float> s_x, sampler s_xSampler`），并做传递闭包；新增 `textureTypeOf()` 并让 `signatureParams()` 复用它 | MSL 没有全局纹理/输出变量：helper 里用 `gl_FragColor`（→`xlatOut.color`）或采样 `bitmap` 都会 "use of undeclared identifier"（`editorBlur.frag`、`editorBlurFast.frag`、`editorWaveforms*.frag`）。`thread XlatMtlFragOut&` 已用 Metal 运行时实测可编译可 link |

改动全部集中在上述文件；未触碰 `MetalUniforms.inc` / `MetalStates.inc` / `BgfxAbi.hx`。

---

## 4. 顺带发现：真实引擎 `.frag` 之前 **12/12 都无法通过 Metal 编译**

任务背景说"翻译出的 MSL 逐个经 Metal 运行时编译，0 错误"——那只覆盖了 **OpenFL 内置 4 个
shader**。把 CNE 真实的 `assets/shaders/**.frag`（12 个）按 `FunkinShader.__initGL()`
的方式拼装（`source/funkin/backend/shaders/FunkinShader.hx:604-700` 的 `fragHeader` /
`vertHeader` / `vertBody` + `#version 120`）后逐个编译，**原先 12 个全部失败**，
错误分成 5 类（对应 §3 的改动 1–5），修完后：

| 状态 | shader |
| --- | --- |
| ✅ 编译 + pipeline link 通过（11） | chromaticAberration、coloredVignette、pixelZoomShader、engine/cameraClip、charterGrid、circleProfilePicture、colorPicker、editorBlur、editorBlurFast、editorWaveforms、updaterShader |
| ❌ 仍失败（1） | `engine/editorWaveformsRainbow.frag` |

唯一遗留：**GLSL 120 的隐式 `int → float` 转换没有建模**。
`uniform ivec2 waveformSize; vec2 wavePixel = vec2(mod(pixelID, waveformSize.x), ...)`
里 `mod(float, int)` 与 `wavePixel / waveformSize`（`vec2 / ivec2`）在 GLSL 120 合法，
但生成的是 `glsl_mod(pixelID, int2(...).x)` / `wavePixel / int2(...)`，MSL 两种都不允许
（模板 `T` 推导冲突 + 向量不许隐式转换）。
**修法建议（未做，因为 `BgfxAbi.hx` 不在我的写入范围）**：
`BgfxAbi.commonHelpers()` 里加一个混合类型重载
`template <typename T, typename U> inline T glsl_mod(T x, U y) { return x - T(y) * floor(x / T(y)); }`
可解决 `mod` 那半；`ivec2` 除法仍需在翻译器里做上下文类型推断（或把 ivec/bvec uniform
直接以 `floatN` 暴露）。该 shader 只用于图表编辑器波形显示，优先级低。

---

## 5. 验证方法与结果（可复现）

1. **既有回归**（在 `metal/haxe` 下，先 `. /tmp/hxenv.sh`）：
   - `haxe -cp . --run lime.graphics.metal.TestTranslate` → **checks: 40, failures: 0, ALL PASSED**
   - `haxe -cp . --run lime.graphics.metal.TestModShaders` → **shaders: 0, failed: 0**（该目录 `assets/shaders`、`mods` 不存在，所以它实际空跑；这也是 12 个真实 shader 的 bug 没被发现的原因）
2. **Metal 运行时编译 + pipeline link**（临时脚手架，全部在 `/tmp`，未写入仓库）：
   - `/tmp/msldump`：翻译器包副本 + `DumpMain.hx` / `CorpusMain.hx`（把真实 `.frag` 拼装并输出 MSL）
   - `/tmp/mslcheck.mm`：用 `-[MTLDevice newLibraryWithSource:options:error:]` 编译两个 stage，
     再按 MSL 里的 `[[attribute(n)]]` 生成 `MTLVertexDescriptor`，调用
     `newRenderPipelineStateWithDescriptor:` **link 顶点/片元**（这一步才能抓出 varying 结构体不匹配）
   - 结果（均 pipeline linked OK）：
     - OpenFL `DisplayObjectShader` VS+FS（5 属性，stride 60）
     - CNE 默认 `FunkinShader` 程序（`vertHeader+vertBody` × `fragHeader+fragBody`，8 属性，stride 96）
     - 真实 `.frag` 语料 **11/12**
     - 顶点 helper 里写 `gl_Position` 的新路径（`thread XlatMtlVaryings&` + 深度修正）
3. **未做**（按任务约束）：没有重新构建 lime、没有运行游戏、没有写第二个仓库文件。

---

## 6. 部署提醒（重要，超出我的写入范围）

翻译器的"真源"是 `metal/haxe/lime/graphics/metal/GLSLToMSL.hx`（我已改），但构建实际读的是：

- `CodenameEngine-1.0.1/source/lime/graphics/metal/GLSLToMSL.hx`（本轮 08:59:45 被同步过一次）
- `~/haxelib-dev/lime-832/src/lime/graphics/metal/GLSLToMSL.hx`（同上）

我最后一批改动（§3 的第 5 项 + 注释）**晚于**那次同步，因此这两份副本目前是旧的。
请再跑一次 `metal/scripts/apply.sh`（或仅把上述文件复制到这两个位置）后重试，
否则 §3-5 的修复不会生效。

---

## 7. 已知遗留限制（不阻塞，供后续排查参考）

1. **`gl_FragCoord`**：翻译成 `xlatIn.position`，而 Metal 片元 `[[position]]` 的 Y 从**顶部**量起、
   GLSL `gl_FragCoord.y` 从**底部**量起。这里才是真正需要 Y 补偿的地方，但补偿需要 viewport 高度
   （GLSL ABI 里没有这个 uniform），当前与 spirv-cross 的 MSL 行为一致（不做补偿）。引擎语料未使用它。
2. **嵌套作用域里再同名遮蔽**：`renameShadowedLocals` 按"声明所在块"改到块尾；若在更内层块里
   重新声明同名变量，改名范围会覆盖整个外层块（极罕见，且只会影响同名变量的取值）。
3. **可写的文件作用域变量**：GLSL 允许改全局变量，MSL program scope 只能是 `constant`（不可写）；
   这类 shader 会编译失败而不是静默错误。
4. **隐式 `int → float`**：见 §4 的唯一遗留 shader。
5. `TestModShaders` 找不到语料目录（`metal/haxe/assets/shaders`、`metal/haxe/mods`），
   等于空跑；建议把它指向 `CodenameEngine-1.0.1/assets/shaders`，否则真实 mod shader 永远没回归。
