# TODO — Metal 渲染后端剩余工作汇总

> 生成时间：2026-10-28（基于当前 `CodenameEngine-1.0.1/` 工作树与 `metal/audit/*` 审计文档复核）
> 范围：`CodenameEngine-1.0.1/metal/`（native + haxe + scripts）、`source/` 中与渲染相关的改动、以及仍然走非 Metal 路径的子系统。
> 结论先行：**CNE 1.0.1 的 2D 渲染（菜单/游戏内/滤镜/视频画面）已全部经 bgfx → Metal 提交，没有 GL context**；
> 剩余缺口集中在四块：①架构上仍是"GL 形状 API 仿真"；②着色器 AOT 未 100%；③少数 2D 条件功能（mask、readPixels）与全部 3D 能力；④诊断/测试工程债。

---

## 0. 已完成基线（对照用，不必再做）

| 项 | 状态 | 证据 |
| --- | --- | --- |
| 全部 269 个 `lime_gl_*` 入口有实现（符号级） | ✅ | `metal/native/MetalBindingsStatus.txt` |
| 2D 渲染全部经 `lime_gl_draw_elements → SubmitDraw → bgfx::submit` | ✅ | `metal/audit/paths.md`、`primitives.md` §1.5（submit 计数 == glDrawElements 计数） |
| 无 GL context / 无 GL 交换链 | ✅ | `metal/scripts/audit-render-paths.sh`；`SDL_WINDOW_OPENGL` 计数 0 |
| 着色器为构建期 GLSL→MSL，运行时零翻译（可枚举的 shader） | ✅ 116 stage 入缓存 | `ShaderAotCache.hx` `COUNT=116` |
| Dusttale 黑屏 / shader 编译失败 / 方向 / 菜单卡顿 | ✅ 已修 | `CodenameEngine-1.0.1/metal/README.md`（开发日志） |
| FBO/滤镜/额外相机/视频画面 | ✅ 已跑通 | 2026-10-28 实机验证（主菜单/Cutscene/选谱/Dusttale/WH） |

---

## 1. P0 — 架构层：OpenFL 仍在用"GL 形状 API"，由 C++ 仿真 GL 状态机

这是**最大的一项"没有完全交给 Metal"**：画面是 Metal 渲染的，但 Haxe 侧仍然走 OpenGL ES 的调用形状
（`lime_gl_*` CFFI，实测 ≈806 次 CFFI/帧），由我们在 C++ 里把 GL 状态机/语义逐条翻译成 bgfx 状态。

| 子项 | 现状 | 工作量/风险 | 参考 |
| --- | --- | --- | --- |
| 1.1 Metal-native `Context3D` 后端（消除 `lime_gl_*` ABI 与 GL 状态仿真） | 未开始；方案 B 已论证 | 2–3 天（Phase 0–2），回归面可控 | `metal/audit/openfl-native.md` §3–§4 |
| 1.2 去掉 bgfx，直连 Metal 提交 | 未开始；已有"同帧 A/B 直连 Metal 回放"原型（逐像素一致） | 1–2 天，收益/风险待评估 | `openfl-native.md` §5.4、`metal/tools/native_replay/` |
| 1.3 lime Haxe 补丁（14 处）需上游化/长期维护 | 本机 `~/haxelib-dev/lime-832` 打补丁；脚本幂等 | 中（跟随 lime 升级） | `metal/scripts/patch_lime_haxe.py`、`README.md` §425 |
| 1.4 GL 语义仿真代码（view/scissor Y 翻转、FBO 附件缓存、常量属性物化、格式猜测、矩阵镜像补偿） | 全部为"GL→bgfx 适配层"，随 1.1/1.2 才能删 | 随 1.1/1.2 | `MetalBindings.cpp`、`MetalNative.inc` |

> 注：当前"方向/镜像"逻辑（`ApplyLateMatrixFlip`）是 GL 语义适配的一部分；1.1 落地后应整块删除。

---

## 2. P0 — 着色器 AOT 未 100%（仍有运行时翻译 / 运行时 MSL 编译）

数据来自 `gen_shader_aot.py` 最近一次生成（`/tmp/cne-aot-regen8.log`）与 `ShaderAotCache.hx`：

| 项 | 数值 | 影响 |
| --- | --- | --- |
| 缓存 stage 总数 | 116（16 vertex / 100 fragment） | — |
| 已预编译 `.metallib` | **112 / 116** | 其余 4 个只能在运行时 `newLibraryWithSource` 编译（首次进入/卡顿风险） |
| 构建期 GLSL→MSL 翻译失败 | **4 个 stage** | 运行时回退到 `GLSLToMSL` 运行时翻译（`CNE_METAL_AOT_RUNTIME_TRANSLATE` 默认 ON） |

### 2.1 构建期仍失败的 4 个 stage（按错误分类）

| # | shader | 错误摘要 |
| --- | --- | --- |
| 1 | `mods/.../cable/fragment` | 依赖 `TWO_PHASE`/`THREE_PHASE` define，未定义时变量缺失（mod 源问题） |
| 2 | `mods/.../firething/fragment` | 使用了未定义的 `noise1()`（mod 源问题） |
| 3 | `mods/.../line/fragment` | 下标作用于非数组/向量 |
| 4 | `mods/.../scaryGlitch/fragment` | `length(float)` 调用有歧义（mod 源码里对分量调用 length） |

> 已修复：`wi.fragment`（`_mtl_u` 重复参数）、`vhs.fragment`（swizzle 复合赋值）、
> 3 个 OpenFL filter shader（`varying vec2 vBlurCoords[5]` 的数组 varying 支持）、
> `DisplacementMapShader`（GLSL 截断构造 `vec2(vec4)` → `.xy`），以及翻译器增强：顶层 helper 前置声明、
> 重复作用域参数折叠、GLSL 数组构造 `float[N](...)` → `{...}`。
> 另修复引擎源码 `source/funkin/backend/shaders/OverlayShader.hx` 的非法 GLSL
> （`base:Vec3` 伪类型、vec4 重载调用参数不匹配），两个 OverlayShader stage 现已可编译。
> 另修正 `assets/shaders/engine/editorWaveformsRainbow.frag`：`uniform ivec2 waveformSize` → `vec2`
> （MSL 没有隐式 int→float 转换，且 Haxe 侧上传的本来就是浮点 [w,h]）。

**TODO**：修 `metal/haxe/lime/graphics/metal/GLSLToMSL.hx`，让这些写法可翻译（每类约 1 处规则），
目标：`failed == 0` 且 `metallib` 覆盖率提升；验收 = 重跑 `python3 metal/scripts/gen_shader_aot.py` 后日志 `failed: 0`。

### 2.2 `.metallib` 只有 112/116

- 原因：本机只有 Command Line Tools，`metal/tools/CompileShaderAot.mm` 对部分 MSL 无法离线编译（日志 `cannot write .metallib`）；
- TODO：逐条修 `cannot write .metallib` 的 MSL 生成（多为 `_mtl_u`/地址空间/模板辅助函数命名问题），
  或引入完整 Xcode/离线 `xcrun metal` 流程；验收 = `metallib compiled : 116 / 116`。
- 影响：4 个 stage 每次冷启动都要在运行时编译 MSL（首次进入某状态可能卡顿，且依赖驱动的编译结果）。

### 2.3 运行时翻译兜底仍然默认开启

`metal/haxe/lime/graphics/metal/ShaderAot.hx:isRuntimeFallbackEnabled()` 默认 true。
**TODO**：2.1 完成后把它改为默认 false（并保留 `CNE_METAL_AOT_RUNTIME_TRANSLATE=1` 仅供调试），
这样"运行时不做任何 GLSL 翻译"才真正成立。

---

## 3. P1 — 2D 条件功能缺口（一旦 mod 用到就会明显坏）

### 3.1 `DisplayObject.mask`（stencil）未接线 —— 最高优先级功能缺口

现状（已核实源码）：

- 交换链和离屏 FBO **都有** `D24S8`（`MetalContext.cpp:702`、`MetalBindings.cpp:4393-4396`），硬件条件具备；
- `lime_gl_stencil_func/op/mask` 写入 `msStencilFront/Back`（`MetalStates.inc:1125+`）；
- `MetalStates.inc` 已有 `MsPackStencil/MsPackTest/MsPackOp*`，且 **`ApplyStencilState()` 已在
  `SubmitDraw()` 中调用**（`MetalBindings.cpp:5992`）→ stencil test/op/ref 已真正下发；
- ~~`lime_gl_color_mask` 只记录、不参与状态~~ → **已修复**：`ToBgfxState()` 现在按
  `state.colorWrite` 生成 `BGFX_STATE_WRITE_R/G/B/A`，mask 形状不会再被不透明地画出来；
- bgfx v167 的 32 位 stencil 字里**没有 write-mask 字段**，`lime_gl_stencil_mask` 的写掩码目前无法下发
  （会影响嵌套 mask，需要在 Metal 侧另做或接受限制）；
- 还需要一个真正使用 `.mask` 的场景做回归验证。

**剩余 TODO**：

1. 评估 stencil write-mask：要么接受限制，要么在 Metal 层自行维护帧内 mask 栈；
2. 用一个使用 `.mask` 的最小场景验证（OpenFL mask 会先画 mask 形状再画内容）；
3. 顺带验证 `lime_gl_stencil_op_separate` 与 back-face 参数。

验收：自建 `Sprite.mask` 场景，遮罩生效且 mask 形状不可见；正常游戏画面无回归。

### 3.2 `read_pixels` 空实现 —— 截图/读回类功能全黑

- `lime_gl_read_pixels`（`MetalBindings.cpp:6895`）直接 `ReportUnimplemented` 返回；
- `lime/_internal/backend/native/NativeWindow.hx:328 readPixels()` 走它 ⇒ `window.readPixels()` 全 0；
- 受影响：游戏内截图、`BitmapData.draw(renderer)`、`Context3D.drawToBitmapData`、mod 自定义读回。

**TODO（中工作量）**：实现延迟读回（`bgfx::readTexture`/Metal blit 到 shared buffer），
语义上允许"晚 1–2 帧"（可在 `glReadPixels` 后阻塞到可用，或按 Lime 用法在下一帧回调）。
验收：`window.readPixels()` 与屏幕内容逐像素一致；截图功能可用。

### 3.3 其他 2D 相关状态语义缺失（低风险，但属"没完全交给 Metal"）

| 项 | 现状 | 位置 |
| --- | --- | --- |
| `glBlendEquation(Separate)` | ✅ 已映射到 `BGFX_STATE_BLEND_EQUATION_SEPARATE`（ADD/SUB/REVSUB/MIN/MAX） | `MetalBindings.cpp ToBgfxState` |
| `glDepthFunc` | ✅ 已映射到 `BGFX_STATE_DEPTH_TEST_*`（仅背缓冲仍按原策略忽略深度） | 同上 |
| `glFrontFace` | ✅ 记录并按 winding 参与 cull 判定 | `MetalContext.h state.frontFace` |
| `glCullFace(mode)` | ✅ 按 FRONT/BACK + winding 选择 `CULL_CW/CULL_CCW` | `ToBgfxCull()` |
| `glColorMask` | ✅ 映射为 `BGFX_STATE_WRITE_R/G/B/A`（同时修好 mask 形状可见问题） | `ToBgfxState` |
| `glBlendColor` | 空实现（bgfx 状态无该项） | `:6607` |
| `glDepthRangef` | 空实现 | `:6638` |
| `glPolygonOffset` | 空实现（bgfx 状态无 depth-bias） | `MetalStates.inc` |
| `glLineWidth` | 忽略（Metal 1px） | `MetalStates.inc` |
| `glSampleCoverage` / MSAA | 未启用 MSAA，恒满覆盖 | `MetalStates.inc:1042,1105` |

剩余项建议随 1.1（native 后端）或确有 3D/mod 需求时再补。

---

## 4. P1 — 3D / Stage3D 能力整体缺失（CNE 官方内容不用，mod 可能用）

| 能力 | 现状 | 说明/位置 |
| --- | --- | --- |
| `GL_TEXTURE_3D` 上传/子上传/存储（含 compressed 3D） | 全部 `ReportUnimplemented` | `MetalTextures.inc`；`bind_texture` 只镜像 `GL_TEXTURE_2D` |
| `glGenerateMipmap` | 静默空实现 | 3D mipfilter 才需要 |
| `glDrawBuffers` / MRT | 报告不支持（view 0 单色附件） | `MetalStates.inc:1182` |
| `glBlitFramebuffer` | 报告不支持（bgfx 需 BLIT_DST + 显式区域） | `MetalTextures.inc:791` |
| `glCopyTexImage2D` / `glCopyTexSubImage2D/3D` | 报告不支持（bgfx 回读异步） | `MetalTextures.inc:926+` |
| `glClearBuffer*` | 报告不支持（bgfx clear 是 view 作用域） | `MetalTextures.inc` |
| Transform feedback 全家族 | 报告不支持（bgfx 无对应概念） | `MetalStates.inc` |
| Uniform block 全家族 + `glBindBufferBase/Range` | 不支持（UBO 折进 `_mtl_u`） | `MetalUniforms.inc:610-634` |
| Program/Shader binary | 不支持（bgfx 用 MSL 源） | `MetalStates.inc` |
| Query 对象（occlusion/timer） | 恒 0 | `MetalStates.inc` |
| `glVertexAttribDivisor`（instancing） | 不支持（bgfx 无 per-attribute divisor） | `MetalTextures.inc` |
| `glFramebufferTextureLayer` | 只挂 layer 0（无 array texture） | `MetalTextures.inc` |
| Indexed GL3 状态查询（`glGetIntegeri_v/64i_v`） | 恒 0 | `MetalUniforms.inc:622` |

> 判断：CNE 2D 内容用不到；是否实现取决于"是否需要跑 away3d/Flx3DView/3D mod"。
> 如果要做，Order 建议：3D 纹理上传 → 状态语义（§3.3）→ MRT/blit → 其余（TF/UBO 等成本高收益低）。

---

## 5. P2 — 已知隐患（会静默出错，不一定立刻可见）

| # | 隐患 | 现状/证据 | 建议 |
| --- | --- | --- | --- |
| 5.1 | **纹理格式回退导致越界读**：`GL_ALPHA/LUMINANCE/LUMINANCE_ALPHA` 等未映射格式退化为 `RGBA8` + 4 B/px（`BytesPerPixel` 默认 4，`MetalBindings.cpp:945`），2D 上传按 `width*4*height` `memcpy`（`MetalNative.inc` `cne_metal_tex_image_2d`） | 默认内容不触发；mod 一旦用 alpha-only 贴图即越界读（内存安全，不只是画面） | 按真实 bpp 计算行宽/总量，未映射格式显式报错；参考 `MetalTextures.inc` 的 `BytesPerTexel` |
| 5.2 | **离屏 view id 上限**：每帧离屏 view 0..199（`MetalBindings.cpp:4513,4661` 到 199 后不再递增） | 单帧目标切换 >200 次时会复用 view id，理论上可导致排序/目标错乱 | 改为按需分配 + 超过阈值时合并/报警，或改用直连 Metal 视图 |
| 5.3 | `framebuffer_renderbuffer` 只记 `renderbuffer` 字段；`renderbuffer_storage` 是空实现（不存尺寸） | mask 若用 renderbuffer 做模板附件将无法创建对应附件 | 随 3.1 一起实现（记录尺寸并创建 stencil 附件） |
| 5.4 | `lime_gl_get_integerv` 只有 2 个 pname（`GL_MAX_TEXTURE_SIZE`/`GL_VIEWPORT`），其它返回 0；`get_integer` 较全 | 部分库/mod 查询状态会拿到 0 | 逐步补齐（参照 `MetalUniforms.inc:158+` 的表） |
| 5.5 | 未注册的死代码：`lime_gl_clear_depth`、`lime_gl_blend_barrier`；`ViewForFramebuffer/nextFramebufferView` 无运行时调用 | 无害，增加审计噪音 | 清理或注明"proto only" |

---

## 6. P2 — 翻译器兼容性：目前依赖"改 mod 源码"才能通过

本轮为让 Dusttale 的 shader 被翻译，曾对 **mod 文件**做最小修补（用户已要求后续不要再改 mod）。
要让"未修改的 mod"直接可用，需要把这些兼容规则做进 `GLSLToMSL`：

| # | 需要支持的写法 | 涉及文件（本轮临时修补） |
| --- | --- | --- |
| 1 | 文件级全局变量（`float ns;`）→ 常量地址空间/静态量处理 | mod `snow/blue/lab_dust/light/dust/waving.frag` |
| 2 | 函数名遮蔽：局部变量 `float min/...` 与内置函数同名 | flxanimate `FilterShader`；mod `iconshader.frag` |
| 3 | 函数式构造 `float(vec3)`、`int(...)` 等标量提取 | mod `light_preprocess/heatlight/impact_frames*/...` |
| 4 | `radians()` 等 OpenFL/GLSL 内置未映射函数 | mod `DropShadow/hue/adjustColor.frag` |
| 5 | 前向声明/函数定义顺序（`noise1` 先用后定义）或自动补声明 | mod `firething.frag` |
| 6 | 非 const 引用绑定向量元素（`v.x` 传引用） | mod `scaryGlitch/vhs`（另 2 个 3D/filter 也报同类） |
| 7 | `mat4` 参数展开/构造（`saturation.frag`） | mod `saturation.frag` |
| 8 | 块注释预处理（已修），多行宏、`#version` 变体等边界继续补测试 | `Preprocessor.hx` |

验收：用**未修改的** mod 目录 + `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` 启动，日志无 `GLSL->MSL failed`，画面正常。

---

## 7. P3 — 工程债：诊断/测试设施

| 项 | 现状 | 建议 |
| --- | --- | --- |
| 诊断开关过多 | 共 **70** 个 `CNE_METAL_*` 环境变量散落在 native/haxe/source | 收敛为统一 `CNE_METAL_DIAG=<mask>` 或配置文件；保留 `DRAWPROBE/UVLOG/FBOLOG/COVERAGE` 等高频项 |
| AOT 失败不阻塞构建 | `gen_shader_aot.py` 只打日志，失败也继续 | 增加 `failed > 0` 时的显式警告/可选 `--strict`，CI 用 strict |
| 无自动化像素回归 | 目前全靠手工截图 + `diff.py` | 用 `metal/tools/native_replay` + 录制帧建立金帧对比（逐像素 diff），纳入脚本 |
| 运行时渲染统计 | `CNE_METAL_STATS`（bgfx draws/submits） | 保留并文档化 |
| 帧录制设施 | `MetalCapture.inc`（`CNE_METAL_CAPTURE_FRAME`）+ `native_replay` | 作为 1.2 的验收工具长期保留 |

---

## 8. 非 Metal 子系统（说明：不属于渲染后端缺口，列出以免误解）

| 子系统 | 实现 | 说明 |
| --- | --- | --- |
| 音频 | OpenAL（macOS 自带 framework，见 `README.md` §191） | CPU/系统音频，与 Metal 无关 |
| 视频解码 | `hxvlc`（VLC，`VIDEO_CUTSCENES` 定义于 `project.xml:124`） | 解码后得到 BitmapData → 以纹理上屏；**上屏仍走 Metal** |
| 图像解码 | libpng（CPU） | 解码后纹理上传走 Metal |
| 字体光栅化 | FreeType（CPU） | 字形位图 → 纹理上传走 Metal |
| 窗口/输入 | SDL3 | 呈现层由 `CAMetalLayer` + bgfx 完成 |
| 游戏逻辑/渲染抽象 | CNE/Flixel/OpenFL（Haxe） | 见 §1 架构项 |

---

## 9. 建议执行顺序（按性价比）

1. **§3.1 mask + §3.3 colorMask/blend equation**（1–2 天，补上 2D 最后的功能缺口，风险中）；
2. **§3.2 readPixels**（1 天，解锁截图/读回）；
3. **§2.1/2.2 翻译器与 metallib 修补**（2–3 天，去掉运行时翻译与首次编译卡顿，并为 §6 铺路）；
4. **§6 翻译器兼容性（不改 mod）**（与 3 合并做，用真实 mod shader 做回归集）；
5. **§1.1 Metal-native Context3D**（2–3 天，真正消除 GL 形状；之后再谈 §1.2 去掉 bgfx）；
6. §4 3D 能力 / §5 隐患 / §7 工程债按需求推进。

---

## 10. 复现与诊断速查

```sh
cd CodenameEngine-1.0.1

# 原语覆盖（哪些 lime_gl_* 真正被调用 / 空实现）
CNE_METAL_COVERAGE=1 CNE_METAL_COVERAGE_OUT=/tmp/cov.txt ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine

# 逐 draw 探针（prog/fbo/tex/size/rt/m0/m5/m13）
CNE_METAL_DRAWPROBE=1 ...

# 屏幕合成翻转判定 / FBO 创建
CNE_METAL_UVLOG=1 ...        CNE_METAL_FBOLOG=1 ...

# AOT 重新生成（看 failed / metallib 覆盖）
python3 metal/scripts/gen_shader_aot.py

# 静态检查 + 构建
./metal/scripts/check-compile.sh --lime "$HOME/haxelib-dev/lime-832" --hxcpp "$HOME/haxelib/hxcpp/4,3,2"
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --build
```

---

## 11. 参考文档

- `metal/audit/primitives.md` — 269 个原语静态/运行时审计（118 完整 / 28 部分 / 52 仅镜像 / 71 空实现；运行时实际 49 个）
- `metal/audit/paths.md` — 渲染特性路径审计（含 R1–R7 风险清单与像素抽查）
- `metal/audit/openfl-native.md` — 消除 GL 形状的原生化方案（方案 B + Phase 0–4 + 直连 Metal 原型）
- `metal/audit/aot-shaders.md` / `aot-coverage.md` — AOT 覆盖与逐 stage 明细
- `metal/README.md` — 构建/补丁/历史问题记录（§63 为遗留问题与结论）
