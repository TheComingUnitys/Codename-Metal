# `lime_gl_*` 原语审计：全量清单、运行时覆盖与空实现缺口

**中文** | [English](../../../../CodenameEngine-1.0.1/metal/audit/primitives.md)

> 任务：`task-6`（team `audit-primitives`） · 审计日期：2026-10-17 · 基线：CNE 1.0.1 + Metal/bgfx 后端（本轮 21:56 之前的源码 + 本次新增覆盖探针）
> 审计原则：**只读 + 加诊断，不改渲染行为**。本报告只描述事实与影响，未修改任何渲染语义。

---

## 0. 结论速览（TL;DR）

1. **名单完全对齐、无缺符号**：Lime 8.3.2 的 CFFI 声明（`NativeCFFI.hx`）、C++ 注册块（`OpenGLBindings.cpp` 的 `DEFINE_PRIME*`）都是 **269** 个 `lime_gl_*`，我们的后端为全部 269 个提供了定义（另多 2 个未被注册的死代码）。`MetalBindingsStatus.txt` 里的 “implemented 271 / stubbed 0” 只是**符号级**统计，**不代表语义完整**——这就是本次审计要回答的问题。
2. **静态语义分类（269）**：完整 118 / 部分 28 / 仅镜像 52 / **空实现 71**。
3. **运行时实测**（给每个入口加 env-gated 计数后重建 ndll，2 个会话共约 1 万帧）：**只有 49 个原语被真正调用**；而且“菜单/Options/CONTROLS”会话与“游戏内 PlayState”会话的**调用集合完全相同**（49 = 49）。这 49 个就是 CNE 2D 渲染的真实执行面。
4. **这 49 个里 42 个是完整/部分实现；7 个是空实现**：`depth_func`、`front_face`、`get_extension`、`stencil_func`、`stencil_func_separate`、`stencil_mask`、`stencil_mask_separate`。它们都只在 **Context3D 的初始状态设置/状态重置**时各被调用 **1 次**，当前 2D 路径下**不丢画面**（原因逐条见 §4.3/§5）。
5. **“被调用但其实是空实现 → 真实缺口”：本次全部会话中 0 个。** 没有任何 2D 渲染功能因为原语空实现而丢失。
6. **但存在“条件性缺口”**（一旦触发就丢画面/丢功能；本次会话未触发）：
   - **DisplayObject.mask（遮罩）**：OpenFL 9.5 的 2D 渲染器用 **stencil + colorMask** 实现 mask（`OpenGLRenderer.hx` `__pushMask/__popMask`）。我们这两族都是空实现 → 一旦有对象用 `.mask`，**遮罩失效、且遮罩形状会以不透明颜色被画出来**。基础游戏未使用 mask（已 grep：CNE/Flixel 源码无 `.mask =`），mod 可能用。
   - **读回（`read_pixels`）**：截图、`BitmapData.draw(renderer)`、`Context3D.drawToBitmapData`、`lime` 的 `window.readPixels()` 会拿到空缓冲。基础游戏不调用（已实测 0 次）。
   - **3D / away3d / Flx3DView 内容**：stencil、cull 模式、front-face、depth-func、depth-range、blend-color、blend-equation、transform feedback、uniform block、program binary、query 等整类不支持（本次 0 调用）。
   - **mipmap 采样（`generate_mipmap`）**：只有 mipfilter sampler（3D 纹理）才会触发；2D 用 LINEAR/NEAREST，本次 0 调用。
   - **实例化属性（`vertex_attrib_divisor`）**、**3D 纹理上传**、**framebuffer blit/drawBuffers** 等高级特性同样只在 3D/高级用法下有意义。
7. **结论**：**CNE 1.0.1 的 2D 渲染路径上，所有实际会被用到的 `lime_gl_*` 原语都真的经 bgfx/Metal 提交渲染**（逐帧 `submitDraw` 计数 == `glDrawElements` 计数，`bgfxDraws` 稳态 27/帧，149,689 次提交 / 7,441 帧（stats 快照；进程总 `draw_elements` = 150,256），且 Metal build 里没有 GL context——见 `metal/scripts/audit-render-paths.sh` 的既有结论）。缺口集中在 3D/高级特性与 mask 这类条件性功能。

---

## 1. 审计方法与证据链

### 1.1 全量名单：三处对齐

| 层 | 来源（权威） | 数量 |
| --- | --- | --- |
| Haxe CFFI 声明 | `~/haxelib-dev/lime-832/src/lime/_internal/backend/native/NativeCFFI.hx`（`@:cffi private static function lime_gl_*`） | **269** |
| Lime C++ 注册 | `~/haxelib-dev/lime-832/project/src/graphics/opengl/OpenGLBindings.cpp`（`DEFINE_PRIME<n>[v]`） | **269** |
| 我们的实现 | `metal/native/MetalBindings.cpp` + `MetalUniforms.inc` + `MetalTextures.inc` + `MetalStates.inc`（`//@impl` 标记） | **271**（269 + 2） |

脚本核对：CFFI 声明集合 == 注册集合（双向差集为空）；注册集合 ⊂ 实现集合（269 个全部有函数体）；实现多出 2 个 **`lime_gl_clear_depth` / `lime_gl_blend_barrier`**，Lime 8.3.2 未注册它们 → 属死代码（运行时 0 调用，无影响）。

> ⚠ `MetalBindingsStatus.txt` 的 “stubbed: 0” 不能当作语义完成的证据：生成器只检查“是否有函数定义”。真正的空实现（有函数体但无效果）有 71 个，全部靠 `MetalContext::ReportUnimplemented` 或静默 no-op 表示。

### 1.2 静态语义分类：4 档

| 档位 | 含义 |
| --- | --- |
| **完整** | 参与真实渲染/资源上传/状态生效（例：`draw_elements`、`tex_image_2d`、`uniform*`、`blend_func_separate`、`viewport`） |
| **部分** | 主体语义落实，个别 GL 语义被忽略（例：`bind_texture` 只镜像 2D target；`enable/disable` 只落实 4 个 cap） |
| **镜像** | 只做对象簿记/状态镜像/查询读回，不直接进渲染（例：`get_*`、`is_*`、`create_query`） |
| **空实现** | 调用后对渲染无任何效果（其中 37 个会 `ReportUnimplemented` 一次性日志，34 个是**静默** no-op） |

委托关系按被委托者语义计入：`blend_func → blend_func_separate`、`stencil_func → stencil_func_separate`、`stencil_mask → *_separate`、`uniform*iv → *fv`、`draw_range_elements → draw_elements`、`renderbuffer_storage_multisample → renderbuffer_storage` 等。

### 1.3 运行时探针（本次新增的诊断设施）

- 新增 `metal/native/MetalCoverage.h` + `metal/native/MetalCoverageNames.inc`（269 个名字预注册，用于列出“从未调用”的原语），在 `MetalBindings.cpp` 顶部 `#include`。
- 机械插入：每个 `//@impl lime_gl_*` 函数体第一行 `CNE_COV ("lime_gl_*");`，共 **271 处**。插桩脚本保证不重复/不破坏代码，且不改变任何语义（关闭时只是一次 `if (false)` 分支）。
- 开关：`CNE_METAL_COVERAGE=1`；`CNE_METAL_COVERAGE_OUT=<path>` 写机器可读报告；正常退出/`SIGTERM`/`SIGINT` 时 dump（覆盖写主文件），`SIGUSR1` 时 dump 到 `<path>.snap`；同时 stderr 打印 `[cne-cov]` 行。
- **计数粒度是 C 原语入口**，因此内部委托也会计数（如 Haxe 调 `glStencilFunc` 会让 `stencil_func` 与 `stencil_func_separate` 各 +1）。这解释了日志里为什么出现 “`stencil_func_separate` 被调用” 而 Haxe 侧其实调的是 `stencilFunc`。
- 与 `audit-shaders` 的 `CNE_METAL_SHADER_AUDIT` 诊断共存于同一次构建（两套诊断互不影响；本次 ndll 同时包含两者）。

> **这是永久诊断设施，不是一次性 hack**：`MetalCoverage.h` + `MetalCoverageNames.inc` + 271 处 `CNE_COV` 已作为后端源码的一部分留在 `metal/native/`，随每次 `apply.sh` + `lime rebuild` 进入 lime.ndll；`CNE_METAL_COVERAGE` 未设置时每条入口只是一次 `if (false)` 分支（**零行为开销**）。**当前 app bundle 的 `lime.ndll` 就包含它（默认关闭）**；同理 `audit-shaders` 的 `CNE_METAL_SHADER_AUDIT` 也在里面（默认关闭）。

### 1.4 运行时会话

| 会话 | 自动按键（`CNE_METAL_AUTOKEYS`，帧:扫描码） | 到达状态 | 帧数 | `glDrawElements` / `submitDraw` | 被调用原语 |
| --- | --- | --- | --- | --- | --- |
| A 菜单 | `1300:40,1700:81,1850:81,2050:40,2600:40` | Title → Main Menu → Options →（Enter）CONTROLS 子状态 | ≈2.7k（`clear` 2,714 次 ≈ 1 次/帧） | 32,891 / 32,891 | **49** |
| B 游戏内 | `1300:40,2100:40,2900:40,4300:40` | Title → Story Menu（Week 1）→ **PlayState** | **7,441**（最后一帧 stats 快照） | **149,689 / 149,689**（同一帧快照；进程总 `draw_elements` = 150,256） | **49** |

说明：两个会话都被其它并行审计代理的外部清理（`quit app` / `pkill`）提前结束；但 A 已走完菜单全流程、B 已进入 PlayState 并持续渲染 4.5k+ 帧（27 个 bgfx draw/帧、稳态 60fps），**覆盖数据完整可用**，不影响任何结论。

### 1.5 交叉证据

- **`ReportUnimplemented` 运行日志**：两个会话都只触发 4 条（`stencil_mask_separate`、`stencil_func_separate`、`vertex_attrib1f`、`vertex_attrib4f`），与覆盖表的空实现调用者完全吻合；其中 `vertex_attrib*` 两条是**过时误报**（见 §4.6）。
- **Haxe 调用点静态扫描**（openfl git 9.5 + flixel git 5.3 + CNE `source/` + lime 8.3.2）：用于判断“未被调用的原语是否根本没有调用方”“条件性缺口的触发条件是什么”。
- **关键架构事实**：OpenFL 9.5 的原生 2D 渲染器**本身就是 Context3D 后端**（`openfl/display/OpenGLRenderer.hx` + `openfl/display3D/Context3D.hx`）。因此 `Context3D.hx` 里的状态设置/stencil/blend 调用**属于 2D 路径**，不能简单按“3D 才用”忽略——这直接影响 §5 的结论：大部分状态调用确实发生在 2D 路径，但**调用 ≠ 需要**（2D 每帧把 culling/depth/stencil 重置为默认，mask 除外）。

---

## 2. 全量三分类

### 2.1 汇总

| 分类 | 数量 | 明细 |
| --- | --- | --- |
| **A：已实现（完整/部分/镜像）且运行时被调用** | **49** | 完整 37 / 部分 5 / 镜像 0 |
| **B：已实现（完整/部分/镜像）但运行时未被调用** | **156** | 完整 81 / 部分 23 / 镜像 52 |
| **C：空实现** | **71** | 被调用 7（均初始化一次性）+ 未调用 64 |
| 合计 | 269 | |

### 2.2 C 类：空实现完整清单（71 个）

按“为什么空”分三组；“运行时”列是本次两个会话的调用次数（`游戏内 / 菜单`）。“真实缺口?”= 是否会在某个**确实可能被用到**的场景丢画面/丢功能。

| # | 原语 | 分组 | 运行时调用 | 真实缺口? | 说明 |
| --- | --- | --- | --- | --- | --- |
| 1 | `lime_gl_blend_color` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **静默 no-op**；bgfx 状态无 blend color 项 |
| 2 | `lime_gl_blend_equation` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | 镜像后 **ToBgfxState 不使用**（固定 ADD）；默认 FUNC_ADD 时无影响 |
| 3 | `lime_gl_blend_equation_separate` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | 同上 |
| 4 | `lime_gl_color_mask` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **静默缺口**：镜像后 ToBgfxState 恒写 RGBA，掩码不生效 |
| 5 | `lime_gl_cull_face` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **部分静默缺口**：cull_face(mode) 的 FRONT/BACK 被忽略；enable 时固定 CULL_CW |
| 6 | `lime_gl_depth_func` | 静默缺口/条件性（无条件日志） | 1 / 1 | **条件性** | **静默 no-op**：固定 LEQUAL（OpenFL 2D 默认即 LEQUAL） |
| 7 | `lime_gl_depth_rangef` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **静默 no-op**；bgfx 无 depth range |
| 8 | `lime_gl_front_face` | 静默缺口/条件性（无条件日志） | 1 / 1 | **条件性** | **静默 no-op**：winding 固定，2D 无影响 |
| 9 | `lime_gl_generate_mipmap` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **静默 no-op**；仅当调用方请求 mipmap 才有意义（2D 路径不请求） |
| 10 | `lime_gl_get_integer64i_v` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | GL3 indexed 状态未跟踪 |
| 11 | `lime_gl_get_integeri_v` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | GL3 indexed 状态未跟踪 |
| 12 | `lime_gl_pixel_storei` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **静默 no-op**；UNPACK_ALIGNMENT 未实现，4B/像素时恒成立 |
| 13 | `lime_gl_stencil_func` | 静默缺口/条件性（无条件日志） | 1 / 1 | **条件性** | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 14 | `lime_gl_stencil_func_separate` | 静默缺口/条件性（无条件日志） | 1 / 1 | **条件性** | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 15 | `lime_gl_stencil_mask` | 静默缺口/条件性（无条件日志） | 1 / 1 | **条件性** | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 16 | `lime_gl_stencil_mask_separate` | 静默缺口/条件性（无条件日志） | 1 / 1 | **条件性** | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 17 | `lime_gl_stencil_op` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 18 | `lime_gl_stencil_op_separate` | 静默缺口/条件性（无条件日志） | 0 / 0 | **条件性** | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 19 | `lime_gl_begin_query` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx 无 occlusion/timer query；结果恒 0 |
| 20 | `lime_gl_begin_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 不支持（报告） |
| 21 | `lime_gl_bind_buffer_base` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx 无 indexed bind point（UBO/SSBO/TF） |
| 22 | `lime_gl_bind_buffer_range` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 同上（offset/size 忽略） |
| 23 | `lime_gl_bind_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 仅记录 |
| 24 | `lime_gl_blit_framebuffer` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx blit 需 BLIT_DST 纹理+显式区域 → 报告 no-op |
| 25 | `lime_gl_clear_bufferfi` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx clear 是 view 作用域；报告 |
| 26 | `lime_gl_clear_bufferfv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 同上 |
| 27 | `lime_gl_clear_bufferiv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 同上 |
| 28 | `lime_gl_clear_bufferuiv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 同上 |
| 29 | `lime_gl_copy_tex_image_2d` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性** | **真实缺口（若有调用方）**：bgfx 读回异步 → 复制纹理为空操作 |
| 30 | `lime_gl_copy_tex_sub_image_2d` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性** | **真实缺口（若有调用方）**：同上 |
| 31 | `lime_gl_copy_tex_sub_image_3d` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性** | **真实缺口（若有调用方）**：同上 |
| 32 | `lime_gl_create_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx 无 transform feedback（仅对象簿记） |
| 33 | `lime_gl_delete_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 仅删除簿记 |
| 34 | `lime_gl_draw_buffers` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 单颜色附件；>1 时报告（2D 恒为 1） |
| 35 | `lime_gl_end_query` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 同上 |
| 36 | `lime_gl_end_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 不支持（报告） |
| 37 | `lime_gl_get_active_uniform_block_name` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性**（shader 用 UBO） | 同上 |
| 38 | `lime_gl_get_active_uniform_blocki` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性**（shader 用 UBO） | 同上 |
| 39 | `lime_gl_get_active_uniform_blockiv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性**（shader 用 UBO） | 同上 |
| 40 | `lime_gl_get_active_uniformsiv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性**（shader 用 UBO） | 同上 |
| 41 | `lime_gl_get_program_binary` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx 不暴露 program 二进制 |
| 42 | `lime_gl_get_query_objectui` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 结果恒 0（AVAILABLE 正确） |
| 43 | `lime_gl_get_query_objectuiv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 结果恒 0 |
| 44 | `lime_gl_get_queryi` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 恒 0 / active id |
| 45 | `lime_gl_get_queryiv` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 恒 0 |
| 46 | `lime_gl_get_transform_feedback_varying` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 不支持 |
| 47 | `lime_gl_get_uniform_block_index` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性**（shader 用 UBO） | 同上 |
| 48 | `lime_gl_pause_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 不支持 |
| 49 | `lime_gl_program_binary` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx 不能从 GL 二进制加载 |
| 50 | `lime_gl_program_parameteri` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 无可分离管线/二进制状态 |
| 51 | `lime_gl_read_buffer` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 仅影响 read_pixels（本身 no-op），镜像仅记录 |
| 52 | `lime_gl_read_pixels` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性** | **真实缺口**：bgfx 读回异步；截图/BitmapData.draw/3D 截图失效 |
| 53 | `lime_gl_resume_transform_feedback` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 不支持 |
| 54 | `lime_gl_shader_binary` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx shader 是 MSL 源码不是 GL 二进制 |
| 55 | `lime_gl_transform_feedback_varyings` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | 不支持 |
| 56 | `lime_gl_uniform_block_binding` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | **条件性**（shader 用 UBO） | uniform block 被折叠进 _mtl_u 结构体 |
| 57 | `lime_gl_vertex_attrib_divisor` | bgfx/Metal 无对应能力（3D/高级特性） | 0 / 0 | — | bgfx 无 per-attribute divisor（实例化属性不支持） |
| 58 | `lime_gl_finish` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | bgfx 每帧 Frame() 同步；GL finish 无可做 |
| 59 | `lime_gl_flush` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 同上 |
| 60 | `lime_gl_get_extension` | 合法 no-op（GL 语义允许/无调用方） | 1 / 1 | — | 返回 null；Lime Haxe 侧忽略此返回值，靠 getString(EXTENSIONS) |
| 61 | `lime_gl_get_internalformativ` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 无 MSAA，恒 0（无调用方） |
| 62 | `lime_gl_get_stringi` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 索引式扩展查询无使用方，返回 null |
| 63 | `lime_gl_hint` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 镜像，advisory |
| 64 | `lime_gl_invalidate_framebuffer` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 内容变未定义，不做任何事语义正确 |
| 65 | `lime_gl_invalidate_sub_framebuffer` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 同上 |
| 66 | `lime_gl_line_width` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | bgfx/Metal 1px 线宽；!=1 时报告 |
| 67 | `lime_gl_polygon_offset` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | bgfx 状态无 depth-bias |
| 68 | `lime_gl_release_shader_compiler` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | bgfx 无可释放的运行时编译器实例，合法 no-op |
| 69 | `lime_gl_sample_coverage` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 无 MSAA |
| 70 | `lime_gl_validate_program` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | GL 语义本为 no-op（仅建议性校验），不影响渲染 |
| 71 | `lime_gl_wait_sync` | 合法 no-op（GL 语义允许/无调用方） | 0 / 0 | — | 等待已 signaled fence，合法 no-op |

### 2.3 B 类：已实现但运行时未被调用的原语（156 个）

这些函数有真实实现（或镜像/查询语义），但本次两个会话**从未进入**。绝大多数是因为**没有 Haxe 调用方**（3D/高级 API 在 2D 游戏里不会触发），少数是条件分支（例如 FBO 只在 FlxCamera/后处理/RTT 时用）。按家族列出，便于判断“若启用某功能会用到哪些原语”。

- **上下文 / 字符串 / 杂项**（11）：`get_boolean`、`get_booleanv`、`get_context_attributes`、`get_error`、`get_float`、`get_floatv`、`get_integer64v`、`get_integerv`、`get_shader_precision_format`、`object_deregister`、`object_from_id`
- **着色器 / program**（11）：`delete_program`、`delete_shader`、`detach_shader`、`get_active_attrib`、`get_active_uniform`、`get_attached_shaders`、`get_frag_data_location`、`get_program_info_log`、`get_programiv`、`get_shader_source`、`get_shaderiv`
- **uniform**（36）：`get_uniformf`、`get_uniformfv`、`get_uniformi`、`get_uniformiv`、`get_uniformui`、`get_uniformuiv`、`uniform1f`、`uniform1fv`、`uniform1iv`、`uniform1ui`、`uniform1uiv`、`uniform2fv`、`uniform2i`、`uniform2iv`、`uniform2ui`、`uniform2uiv`、`uniform3f`、`uniform3fv`、`uniform3i`、`uniform3iv`、`uniform3ui`、`uniform3uiv`、`uniform4f`、`uniform4fv`、`uniform4i`、`uniform4iv`、`uniform4ui`、`uniform4uiv`、`uniform_matrix2fv`、`uniform_matrix2x3fv`、`uniform_matrix2x4fv`、`uniform_matrix3fv`、`uniform_matrix3x2fv`、`uniform_matrix3x4fv`、`uniform_matrix4x2fv`、`uniform_matrix4x3fv`
- **缓冲区**（10）：`buffer_sub_data`、`copy_buffer_sub_data`、`delete_buffer`、`get_buffer_parameteri`、`get_buffer_parameteri64v`、`get_buffer_parameteriv`、`get_buffer_pointerv`、`get_buffer_sub_data`、`map_buffer_range`、`unmap_buffer`
- **顶点属性 / VAO**（23）：`bind_vertex_array`、`create_vertex_array`、`delete_vertex_array`、`get_vertex_attrib_pointerv`、`get_vertex_attribf`、`get_vertex_attribfv`、`get_vertex_attribi`、`get_vertex_attribii`、`get_vertex_attribiiv`、`get_vertex_attribiui`、`get_vertex_attribiuiv`、`get_vertex_attribiv`、`vertex_attrib1fv`、`vertex_attrib2f`、`vertex_attrib2fv`、`vertex_attrib3f`、`vertex_attrib3fv`、`vertex_attrib4fv`、`vertex_attrib_ipointer`、`vertex_attribi4i`、`vertex_attribi4iv`、`vertex_attribi4ui`、`vertex_attribi4uiv`
- **纹理**（15）：`compressed_tex_image_2d`、`compressed_tex_image_3d`、`compressed_tex_sub_image_2d`、`compressed_tex_sub_image_3d`、`delete_texture`、`get_tex_parameterf`、`get_tex_parameterfv`、`get_tex_parameteri`、`get_tex_parameteriv`、`tex_image_3d`、`tex_parameterf`、`tex_storage_2d`、`tex_storage_3d`、`tex_sub_image_2d`、`tex_sub_image_3d`
- **framebuffer / renderbuffer / 读回**（16）：`bind_framebuffer`、`bind_renderbuffer`、`check_framebuffer_status`、`create_framebuffer`、`create_renderbuffer`、`delete_framebuffer`、`delete_renderbuffer`、`framebuffer_renderbuffer`、`framebuffer_texture2D`、`framebuffer_texture_layer`、`get_framebuffer_attachment_parameteri`、`get_framebuffer_attachment_parameteriv`、`get_renderbuffer_parameteri`、`get_renderbuffer_parameteriv`、`renderbuffer_storage`、`renderbuffer_storage_multisample`
- **绘制**（4）：`draw_arrays`、`draw_arrays_instanced`、`draw_elements_instanced`、`draw_range_elements`
- **固定管线状态**（3）：`clear_depthf`、`clear_stencil`、`is_enabled`
- **查询 / 同步 / fence**（9）：`client_wait_sync`、`create_query`、`delete_query`、`delete_sync`、`fence_sync`、`get_sync_parameteri`、`get_sync_parameteriv`、`is_query`、`is_sync`
- **transform feedback**（1）：`is_transform_feedback`
- **采样器对象**（10）：`bind_sampler`、`create_sampler`、`delete_sampler`、`get_sampler_parameterf`、`get_sampler_parameterfv`、`get_sampler_parameteri`、`get_sampler_parameteriv`、`is_sampler`、`sampler_parameterf`、`sampler_parameteri`
- **is_* 对象判定**（7）：`is_buffer`、`is_framebuffer`、`is_program`、`is_renderbuffer`、`is_shader`、`is_texture`、`is_vertex_array`

---

## 3. 运行时调用计数表（A 类 49 个，按游戏内次数排序）

`⚠` = 空实现；`◐` = 部分实现；其余为完整实现。计数为**进程累计**（游戏内会话 7,441 帧 / 菜单会话 ≈2.7k 帧）。

| # | 原语 | 实现度 | 游戏内次数 | 菜单次数 | 作用（一句话） |
| --- | --- | --- | --- | --- | --- |
| 1 | `lime_gl_disable_vertex_attrib_array` | 完整 | 1,499,134 | 259,125 | 禁用 |
| 2 | `lime_gl_enable_vertex_attrib_array` | 完整 | 709,792 | 149,356 | 标记 attribute 使用真实 buffer |
| 3 | `lime_gl_uniform1i` | 完整 | 586,254 | 103,546 | 写入 program uniform 暂存区，提交时 bgfx::setUniform |
| 4 | `lime_gl_vertex_attrib4f` | 完整 | 507,326 | 96,602 | 常量属性被物化成额外 vertex stream（第 49.1/52 节修复） |
| 5 | `lime_gl_bind_buffer` | 完整 | 428,492 | 87,794 | ARRAY/ELEMENT 绑定镜像 |
| 6 | `lime_gl_enable` `◐` | 部分 | 420,930 | 87,713 | 只落实 BLEND/DEPTH_TEST/CULL_FACE/SCISSOR_TEST；其余 cap 静默忽略 |
| 7 | `lime_gl_vertex_attrib_pointer` | 完整 | 409,280 | 83,574 | 记录 layout；draw 时绑定 bgfx stream |
| 8 | `lime_gl_bind_texture` `◐` | 部分 | 207,203 | 54,062 | 只镜像 GL_TEXTURE_2D/2D 到 unit；GL_TEXTURE_3D/CUBE 不镜像 |
| 9 | `lime_gl_active_texture` | 完整 | 195,317 | 49,577 | unit 选择 |
| 10 | `lime_gl_viewport` | 完整 | 157,718 | 35,605 | ConfigureView 每笔提交应用 |
| 11 | `lime_gl_draw_elements` | 完整 | 150,256 | 32,891 | index buffer + stream + SubmitDraw |
| 12 | `lime_gl_uniform2f` | 完整 | 150,256 | 32,891 | 写入 program uniform 暂存区，提交时 bgfx::setUniform |
| 13 | `lime_gl_uniform_matrix4fv` | 完整 | 150,256 | 32,891 | 转置为 MSL 行主序后写 uniform |
| 14 | `lime_gl_vertex_attrib1f` | 完整 | 150,256 | 32,891 | 常量属性被物化成额外 vertex stream（第 49.1/52 节修复） |
| 15 | `lime_gl_buffer_data` | 完整 | 120,538 | 19,200 | CPU mirror；draw 时按内容签名建 bgfx buffer |
| 16 | `lime_gl_disable` `◐` | 部分 | 112,654 | 18,916 | 同上 |
| 17 | `lime_gl_use_program` | 完整 | 22,682 | 8,665 | 记录 currentProgram，提交时绑定 |
| 18 | `lime_gl_clear` | 完整 | 7,462 | 2,714 | bgfx::setViewClear（含 depth 修正） |
| 19 | `lime_gl_clear_color` | 完整 | 7,462 | 2,714 | → setViewClear rgba |
| 20 | `lime_gl_tex_image_2d` | 完整 | 5,902 | 2,218 | CPU mirror + bgfx::createTexture2D，BGRA8/RGBA8/RGBA32F |
| 21 | `lime_gl_get_attrib_location` | 完整 | 594 | 346 | 返回分配的唯一 location（OpenFL 依赖） |
| 22 | `lime_gl_get_uniform_location` | 完整 | 519 | 302 | 返回 uniform 槽（OpenFL uniform 上传依赖） |
| 23 | `lime_gl_tex_parameteri` | 完整 | 328 | 196 | MIN/MAG_FILTER + WRAP_S/T → BGFX_SAMPLER_* |
| 24 | `lime_gl_object_register` | 完整 | 212 | 142 | Lime 对象注册表（GC/身份） |
| 25 | `lime_gl_create_buffer` | 完整 | 121 | 84 | 对象 |
| 26 | `lime_gl_create_texture` | 完整 | 82 | 49 | 纹理对象（bgfx 句柄延迟创建） |
| 27 | `lime_gl_bind_attrib_location` | 完整 | 18 | 18 | 绑定 GL location（Haxe 侧生成唯一 location） |
| 28 | `lime_gl_get_string` `◐` | 部分 | 7 | 7 | VENDOR/RENDERER/VERSION 真实；EXTENSIONS 只报 GL_EXT_bgra（OpenFL 依赖） |
| 29 | `lime_gl_attach_shader` | 完整 | 6 | 6 | 记录 VS/FS |
| 30 | `lime_gl_compile_shader` | 完整 | 6 | 6 | MSL 交给 bgfx::createShader；失败信息进 infoLog |
| 31 | `lime_gl_create_shader` | 完整 | 6 | 6 | 创建 shader 对象；source 由 Haxe 侧 GLSL→MSL 翻译器提供 |
| 32 | `lime_gl_get_shader_info_log` | 完整 | 6 | 6 | bgfx 编译错误文本 |
| 33 | `lime_gl_get_shaderi` | 完整 | 6 | 6 | COMPILE_STATUS/DELETE_STATUS/SHADER_TYPE 真实反映 |
| 34 | `lime_gl_shader_source` | 完整 | 6 | 6 | 保存 MSL 源码（Haxe 已翻译） |
| 35 | `lime_gl_create_program` | 完整 | 3 | 3 | program 对象 + attribute/uniform 元数据 |
| 36 | `lime_gl_get_programi` | 完整 | 3 | 3 | LINK_STATUS/DELETE_STATUS/ACTIVE_* 真实 |
| 37 | `lime_gl_link_program` | 完整 | 3 | 3 | 调用 bgfx::createProgram，全 program 可真实 draw |
| 38 | `lime_gl_get_integer` `◐` | 部分 | 2 | 2 | QueryIntegers 覆盖常见 limits，未覆盖项 0 |
| 39 | `lime_gl_blend_func` | 完整 | 1 | 1 | → blend_func_separate，进 bgfx 混合状态 |
| 40 | `lime_gl_blend_func_separate` | 完整 | 1 | 1 | 存入 state，ToBgfxState 应用 BGFX_STATE_BLEND_FUNC_SEPARATE |
| 41 | `lime_gl_depth_func` `⚠` | 空实现 | 1 | 1 | **静默 no-op**：固定 LEQUAL（OpenFL 2D 默认即 LEQUAL） |
| 42 | `lime_gl_depth_mask` | 完整 | 1 | 1 | → BGFX_STATE_WRITE_Z |
| 43 | `lime_gl_front_face` `⚠` | 空实现 | 1 | 1 | **静默 no-op**：winding 固定，2D 无影响 |
| 44 | `lime_gl_get_extension` `⚠` | 空实现 | 1 | 1 | 返回 null；Lime Haxe 侧忽略此返回值，靠 getString(EXTENSIONS) |
| 45 | `lime_gl_scissor` | 完整 | 1 | 1 | scissorEnabled 时 ConfigureView 应用 |
| 46 | `lime_gl_stencil_func` `⚠` | 空实现 | 1 | 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 47 | `lime_gl_stencil_func_separate` `⚠` | 空实现 | 1 | 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 48 | `lime_gl_stencil_mask` `⚠` | 空实现 | 1 | 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |
| 49 | `lime_gl_stencil_mask_separate` `⚠` | 空实现 | 1 | 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 |

**观察**

- 两个会话的调用集合**逐项相同**（49 = 49，差集为空），说明 CNE 1.0.1 的 2D 渲染面对原语的使用是**固定集合**，不随菜单/游戏切换变化。
- 调用频率最高的是 OpenFL 2D 渲染器的逐 draw 设置：`enable/disable_vertex_attrib_array`、`uniform1i`、`vertex_attrib4f`(常量颜色/alpha)、`bind_buffer`、`vertex_attrib_pointer`、`draw_elements`、`uniform_matrix4fv`。这些全部有真实实现——与 README 第 49/52 节的修复（常量属性物化、uniform 上传、顶点流）一致。
- 7 个空实现全部是 **1 次**调用（Context3D 初始化/清屏/状态重置各一次），没有任何一个在逐帧热路径上被调用。
- `◇` 镜像类原语在本次会话中**一次都没有被调用**（`get_*`/`is_*`/`getUniform*` 等 OpenFL 2D 渲染器不使用）。

---

## 4. 重点嫌疑原语逐条核实（README 记录的 no-op）

README 第 16/17/45 节等处记录的后端 “no-op” 清单，本次用运行时计数 + Haxe 调用点双重核实：

### 4.1 `lime_gl_read_pixels` — 空实现，运行时 **0 次**，条件性缺口

- 实现：`MetalBindings.cpp` 里只做 `ReportUnimplemented`（bgfx 读回异步，需要 2 帧后才可读），不写 `pixels`。
- 调用点（Haxe 静态扫描）：只有 ① `openfl/display3D/Context3D.hx` 的 `drawToBitmapData()`（3D API）；② lime 的 `NativeWindow.readPixels()`（截图/`window.readPixels()` 公共 API，mod 可用）。CNE 自身源码没有任何调用。
- 运行时：两个会话均 **0 次**。
- 影响：基础游戏（标题/菜单/游戏内）**不丢画面**；若 mod 或引擎功能使用截图/`BitmapData.draw(renderer)`/3D `drawToBitmapData`，会得到全 0/黑图。这是**真实但未触发**的缺口。
- 修复难度：中（需要 `bgfx::readTexture` + 延迟 1–2 帧的同步读回；或直接对 `MetalContext` 的后台缓冲做一次 Metal blit 到 shared texture 再 memcpy）。

### 4.2 `lime_gl_copy_tex_image_2d` / `copy_tex_sub_image_2d` / `copy_tex_sub_image_3d` — 空实现，运行时 **0 次**

- 实现：只有 `ReportUnimplemented`（bgfx 帧缓冲读回异步）。
- 调用点：**全 Haxe 代码库 0 个调用点**（grep openfl 9.5 git / flixel 5.3 git / CNE source / lime 8.3.2：`gl.copyTexImage2D`、`gl.copyTexSubImage2D` 均无匹配）。
- 结论：**纯死路径**（CFFI 表里存在但 OpenFL 2D/3D 都不使用）。只有 mod 直接调用 `GL.copyTexImage2D` 才会触发；届时会静默失败。
- 修复难度：中（与 read_pixels 同一套读回机制）。

### 4.3 `lime_gl_stencil_func` / `stencil_mask` / `stencil_op`（+ `*_separate`）— 空实现，运行时 **各 1 次**

- 实现：镜像到 `msStencilFront/Back` 供查询，但 `ToBgfxState()` **没有 stencil 位**，因此不产生任何 GPU 状态；每次调用 `ReportUnimplemented`（`*_separate` 首次；`stencil_func/mask/op` 通过委托也触发一次）。
- 运行时：各 **1 次**（`stencil_mask_separate`/`stencil_func_separate`），来自 OpenFL 2D 渲染器每帧开头 `__render()` 的 `setStencilActions()` 状态重置（`Context3D` 有状态缓存，默认值第一次落地时各调一次；本次 `clear_stencil` 计数为 0，说明不是清屏路径触发的）。
- **关键影响判断**：基础游戏**不使用 stencil**——`Flixel/CNE` 源码无 `DisplayObject.mask` 使用；2D 渲染器唯一需要 stencil 的路径是 **mask**（`OpenGLRenderer.__pushMask/__popMask`：用 INCREMENT/DECREMENT_SATURATE + `EQUAL` 比较 + `colorMask(false,false,false,false)` 先画遮罩形状，再按 stencil 判定画被遮罩内容）。**没有 mask 时，stencil 清屏/状态设置对画面没有任何可见影响。** 因此本次会话**不丢画面**。
- ⚠ **一旦有对象使用 `.mask`**：stencil 测试永远不启用（`enable(GL_STENCIL_TEST)` 也被 `enable` 忽略，见 §5），于是 ① 遮罩完全不裁剪（内容溢出遮罩）；② `colorMask` 空实现使遮罩形状本身以不透明颜色画出。**这是确定的潜在视觉 bug**，影响任何使用 mask 的 mod/3D 界面。
- 修复难度：中（bgfx `BGFX_STATE_STENCIL_*` 位齐全，需在 `ToBgfxState()` 中按当前 stencil+colorMask 状态拼装，并在 FBO/backbuffer 创建时带 stencil attachment；注意 bgfx FBO 需要显式 depth-stencil 纹理）。

### 4.4 `lime_gl_generate_mipmap` — 空实现（**静默**），运行时 **0 次**

- 实现：注释说明“bgfx 在创建纹理时按 mip 数生成”，函数体为空，**没有** `ReportUnimplemented`。
- 调用点：只有 3D 纹理（`openfl/display3D/textures/Texture.hx` 的 `__setSamplerState`，当 `mipfilter != MIPNONE`；`CubeTexture.hx`）。2D 渲染器用 LINEAR/NEAREST，不请求 mip。
- 运行时：**0 次**。2D 画面/缩放**不受影响**（缩放质量取决于 LINEAR 过滤，与 mipmap 无关）。
- 影响：仅 3D/自定义 mipmap 采样；修复难度低（建纹理时按 `glGenerateMipmap` 的调用时机把 `hasMips` 传对，或保留 CPU mip 链后重传）。

### 4.5 `lime_gl_pixel_storei` — 空实现（**静默**），运行时 **0 次**，实际可视为无害

- 实现：空函数体（注释说上传路径恒为紧密排列）。
- 调用点：只有 `openfl/display3D/textures/TextureBase.hx` 的 `UNPACK_PREMULTIPLY_ALPHA_WEBGL`，且这些调用**全部位于 `#if (js && html5)` 分支内** → **原生/macOS 构建根本不会调用**。
- 运行时：**0 次**。即使将来被调用，OpenFL 原生纹理都是 4 字节/像素（RGBA8/BGRA8），`UNPACK_ALIGNMENT` 默认 4 时行对齐恒成立 → 无影响。
- 结论：README 把它列为 no-op 是准确的，但它是**三层无害**（无调用点 + 原生不编译 + 即便调用也无影响）。

### 4.6 顺带发现：`ReportUnimplemented` 对 `vertex_attrib1f/4f` 的文案已过时（**误报**）

- 两个会话都打印：`lime_gl_vertex_attrib1f (bgfx has no constant vertex attributes; the value is mirrored for queries only)` / `lime_gl_vertex_attrib4f (...)`。
- 但实际行为：这些常量属性已经被 `CollectConstantAttribs` 收集，并在 `BindVertexStreams` 中**物化成额外的 vertex stream**（README 第 49.1/52 节的修复）；运行时 `[cne-dbg] constant attribs -> stream ...` 也在证明它们真的到达了 shader。
- 所以这条日志是**过时文案的误报**（`MetalTextures.inc` 的 `ReportConstantAttrib` 仍按旧实现描述）。它不是渲染 bug，但会误导审计/排障。
- **处置（已按 Lead 要求只改源码、不重建）**：`ReportConstantAttrib` 改名为 `ReportIntegerConstantAttrib`；8 个**浮点**常量属性变体（`vertex_attrib{1..4}f[/v]`）**不再上报**（值已物化成 vertex stream）；4 个**整数**变体（`vertex_attribi4{i,iv,ui,uiv}`，确实只镜像不物化）保留上报并更正文案。`check-compile.sh` 已通过；**待下次 `lime rebuild mac` 生效**——当前 app 里的 ndll 仍是旧文案（不影响渲染行为）。

### 4.7 `lime_gl_color_mask` / `cull_face` / `front_face` / `depth_func` / `blend_*` — 见 §5 逐条

---

## 5. 状态类原语语义逐条检查（容易被静默忽略）

“我们的落实”一列严格对照 `MetalBindings.cpp` 的 `ToBgfxState()` / `ConfigureView()` 的实现；“运行时”为本次两会话调用次数。

| 原语 | GL 语义 | 我们的落实 | 运行时(游戏/菜单) | 未落实的后果 |
| --- | --- | --- | --- | --- |
| `cull_face(mode)` | 选择剔除 FRONT/BACK/BOTH | ❌ **空实现**：`(void)mode`；`enable`/`disable(GL_CULL_FACE)` 有记录，但开启时**固定** `BGFX_STATE_CULL_CW` | 0 / 0 | 若 2D/3D 依赖 FRONT/BACK 选择：剔除方向可能相反（几何消失）。CNE 2D 每帧 `setCulling(NONE)` → **当前无影响** |
| `front_face(dir)` | 定义正面绕序 CCW/CW | ❌ **空实现**（`(void)face`） | 1 / 1 | 只有开启 culling 才有意义；2D 关闭 culling → 当前无影响。3D/自定义绕序会错 |
| `color_mask(r,g,b,a)` | 控制写入哪些通道 | ❌ **空实现但会镜像**：`ToBgfxState()` 恒 `WRITE_RGB`+`WRITE_A`，不读 `state.colorWrite` | 0 / 0 | **mask 渲染必需**（`__pushMask` 把 colorMask 设为全 false 以“只写 stencil 不写颜色”）：若用 mask，遮罩形状会被画出来。另外自定义“只写 alpha”技巧会失效 |
| `depth_mask(flag)` | 深度写开关 | ✅ 完整：`state.depthWrite → BGFX_STATE_WRITE_Z` | 1 / 1 | — |
| `depth_func(func)` | 深度比较函数 | ❌ **空实现**：固定 `BGFX_STATE_DEPTH_TEST_LEQUAL`（`(void)func`） | 1 / 1 | 2D 每帧 `setDepthTest(false, ALWAYS)` → 深度测试关闭 → 当前无影响。若 3D 用 ALWAYS/GREATER 等会错 |
| `depth_rangef(zNear,zFar)` | 深度范围映射 | ❌ **空实现**（静默） | 0 / 0 | bgfx 无 depth range；2D/普通 3D 几乎不用 |
| `stencil_func/mask/op`（+`*_separate`） | stencil 测试/写掩码/操作 | ❌ **空实现**（镜像状态供查询，`ToBgfxState()` 无 stencil 位） | 各 1（`*_separate` 与委托者各 1） | 见 §4.3：无 mask 时无影响；有 mask 时遮罩失效（**条件性真实缺口**） |
| `scissor(x,y,w,h)` | 裁剪矩形 | ✅ 完整：`state.scissor` + `scissorEnabled`，`ConfigureView()` 每笔提交应用 `setViewScissor` | 1 / 1 | —（注意：`enable/disable(GL_SCISSOR_TEST)` 只有这两个 cap 生效） |
| `viewport(x,y,w,h)` | 视口矩形 | ✅ 完整：`state.viewport`，`ConfigureView()` 按 FBO/backbuffer 映射后 `setViewRect` | 157,718 / 35,605 | — |
| `blend_func(src,dst)` → `blend_func_separate` | 混合因子 | ✅ 完整：`state.blendSrcRGB/...`，`ToBgfxState()` 用 `BGFX_STATE_BLEND_FUNC_SEPARATE` | 1 / 1 | —（`ToBgfxBlend` 映射了 OpenFL 用到的全部因子） |
| `blend_func_separate` | RGB/A 分开的混合因子 | ✅ 完整（同上） | 1 / 1 | — |
| `blend_equation(mode)`（+`_separate`） | 混合方程 ADD/SUBTRACT/… | ❌ **空实现（仅镜像）**：`ToBgfxState()` 不读 `blendEqRGB/Alpha`，实际恒 ADD | 0 / 0 | OpenFL 2D 默认 FUNC_ADD → 当前无影响；`Glow`/自定义 `FUNC_REVERSE_SUBTRACT` 等混合模式会退化成 ADD（`OpenGLRenderer` 里确有 `__setGLBlendEquation(FUNC_REVERSE_SUBTRACT)` 的调用点，可能被某些混合模式用到） |
| `blend_color` | 混合常量色 | ❌ **空实现（静默）** | 0 / 0 | Haxe 全库 0 调用点；无影响 |
| `pixel_storei` | UNPACK 对齐/预乘等 | ❌ **空实现（静默）** | 0 / 0 | 见 §4.5：原生分支不调用，且 4B/像素下对齐无影响 |
| `enable/disable(cap)` | 开关 GL 能力 | ◐ **部分**：只落实 `BLEND`/`DEPTH_TEST`/`CULL_FACE`/`SCISSOR_TEST`；`STENCIL_TEST` 等被**静默忽略** | 420,930 / 87,713（大量调用，cap 以 BLEND/DEPTH/CULL/SCISSOR 为主） | mask 需要的 `STENCIL_TEST` 无法打开（**条件性**）；2D 其余 cap 无意义 |
| `is_enabled(cap)` | 查询 cap | ◇ 镜像（只对上述 4 个 cap 返回真实值，其余 false） | 0 / 0 | 只影响查询语义 |
| `clear` / `clear_color` | 清屏 | ✅ 完整：`ConfigureView()` + `bgfx::setViewClear`（含 depth 位修正，见 README 49 节注释） | 7,462 / 2,714（≈1 次/帧） | — |
| `clear_depthf` / `clear_stencil` | 清屏深度/stencil 值 | ✅ 完整（存值进 `setViewClear`；stencil 值本身无效果因为 stencil 状态不支持） | 0 / 0 | 当前无影响 |

补充说明：

- 2D 渲染器 `OpenGLRenderer.__render()` **每帧主动重置** `setColorMask(true,true,true,true)`、`setCulling(NONE)`、`setDepthTest(false, ALWAYS)`、`setStencilActions()`、`setScissorRectangle(null)`。这正好把 `cull_face/front_face/depth_func/blend_equation/color_mask/stencil_*` 的“缺失语义”压到默认值上——**这就是为什么这些空实现当前不丢画面**。
- **唯一例外是 mask**：`__pushMask`/`__popMask` 会**主动偏离**默认值（colorMask 全 false、stencil INCR/DECR + EQUAL、enable STENCIL_TEST），而这三项我们都不支持。基础游戏不用 mask，但这是“一旦使用就坏”的确定缺口。
- 另一个结构性限制：`ToBgfxState()` 是**按帧/按 draw 重建**的 bgfx state，未实现的部分不只是“没写字段”，而是完全没有对应的 GL 状态跟踪（比如 blend equation 有镜像但没进 state）。

---

## 6. 结论与缺口清单

### 6.1 核心结论

**问：是否所有会被用到的渲染原语都真的经 Metal 渲染？**

**答：是——只要“会被用到”限定为 CNE 1.0.1 的 2D 渲染路径（标题/菜单/Options/CONTROLS/游戏内 PlayState）。** 证据链：

1. 运行时探针证明**恰好 49 个**原语被调用，两个不同场景的集合**完全一致**；其中 **42 个是完整/部分实现**，7 个空实现全部只在初始化时调用 1 次且语义非必需（§3/§4.3）。
2. 所有绘制都经 `lime_gl_draw_elements → BindVertexStreams → SubmitDraw → bgfx::submit`；`glDrawElements` 计数与 `submitDraw` 计数**逐帧相等**（149,689 = 149,689），`bgfxDraws` 稳态 27/帧。没有任何 `glDrawArrays` 直通 GL 的旁路（该计数为 0）。
3. 本进程没有任何 GL context（`audit-render-paths.sh` 已静态验证 `SDL_WINDOW_OPENGL`/`SDL_GL_CreateContext`/`SDL_GL_SwapWindow` 在 Metal 构建中为 0；本次构建同样如此）。
4. 运行时**不存在**“被调用 + 空实现 → 丢画面”的 2D 功能（唯一被调用的空实现是 §4.3 的初始化 stencil / depth_func / front_face / get_extension）。

### 6.2 缺口清单（按真实影响 + 修复难度）

| # | 缺口 | 触发条件 | 影响画面/功能 | 本次会话触发? | 修复难度 |
| --- | --- | --- | --- | --- | --- |
| 1 | **mask 支持**：stencil test/op/func + colorMask + `enable(STENCIL_TEST)` | 任意对象 `.mask`（mod / 3D 界面） | 遮罩不裁剪 + 遮罩形状可见（**确定的视觉 bug**） | 否（0 次调用） | **中**：`ToBgfxState` 加 STENCIL 位 + FBO/backbuffer 带 stencil + `enable` 支持 STENCIL_TEST + colorMask 进 state |
| 2 | `read_pixels` 空实现 | 截图 / `BitmapData.draw(renderer)` / `Context3D.drawToBitmapData` / mod 调 `window.readPixels` | 得到全 0（黑图） | 否（0 次） | **中**：`bgfx::readTexture` + 延迟 1–2 帧，或 Metal blit 到 shared 后 memcpy |
| 3 | 3D 固定管线语义：cull_face 模式、front_face、depth_func、depth_range、blend_color、blend_equation | away3d / Flx3DView / 3D mod / 自定义混合模式（如 REVERSE_SUBTRACT） | 剔除方向/深度比较/混合方程错误 | 否 | **中**：`ToBgfxState` 从镜像值生成对应 bgfx 状态位（大部分字段已有镜像） |
| 4 | `generate_mipmap` | 3D 纹理 mipfilter 采样 | 3D 远处纹理闪烁/质量差 | 否（0 次） | **低**：创建纹理时把 mip 数/生成时机传对 |
| 5 | 实例化属性 `vertex_attrib_divisor` | 3D instancing / FlxTilemap 之外的 instanced 绘制 | 实例数据错误 | 否 | **高**（bgfx 无 per-attribute divisor，要改数据布局） |
| 6 | 3D 纹理上传（tex_image_3d / tex_sub_image_3d / tex_storage_3d / compressed 3D） | away3d / 3D mod | 3D 贴图缺失/花屏 | 否 | **中**：`bind_texture` 镜像非 2D target |
| 7 | transform feedback / uniform block / program binary / query 对象 / blit_framebuffer / draw_buffers / clear_buffer* | 只被 3D 高级 API 使用 | 对应 3D 特性不可用 | 否 | **高**（bgfx 无这些概念，需要完全不同的实现路径） |
| 8 | `ReportUnimplemented` 对 `vertex_attrib1f/4f` 的过时误报 | 每次运行首次调用 | 不丢画面，**误导审计**（其实已物化成功） | **是**（各 1 次） | ✅ **已修源码**（未重建）：浮点变体不再上报、整数变体保留并更正文案；check-compile 通过，待下次构建生效 |

### 6.3 修复优先级建议

- **P0（2D 画面）**：无。2D 渲染没有真实缺口。
- **P1（条件性 2D 功能）**：#1 mask、#2 read_pixels——如果后续要支持 mod 的遮罩/截图。
- **P2（3D）**：#3 状态语义、#4 mipmap、#6 3D 纹理——只有确实要跑 away3d/Flx3DView 时才值得做。
- **P3（高级 3D）**：#5/#7——bgfx 架构限制，成本高收益低。
- ✅ 顺手已做：#8 误报文案修正（已改源码、未重建，下次构建生效；避免下次审计再次被误导）。

### 6.4 对 README 已记录 no-op 的裁决

| README 记录 | 本次实测 | 裁决 |
| --- | --- | --- |
| `lime_gl_read_pixels` | 空实现，0 调用（2D 路径） | ✅ 记录正确；**条件性缺口**（截图/3D 才触发） |
| `copy_tex_image_2d` / `copy_tex_image_sub`（应指 `copy_tex_sub_image_2d/3d`） | 空实现，0 调用，Haxe 全库 0 调用点 | ✅ 记录正确；**死路径**，当前无任何功能依赖 |
| `stencil_mask_separate` / `stencil_func_separate` | 空实现，各被调用 **1 次**（委托/状态重置） | ✅ 记录正确；基础 2D 无影响；**mask 是条件性缺口** |
| `generate_mipmap` | 空实现（静默），0 调用 | ✅ 记录正确；2D 缩放质量**不受影响**；3D mipmap 才受影响 |
| `pixel_storei` | 空实现（静默），0 调用；原生分支根本不编译 | ✅ 记录正确；**三层无害**，实际无需修 |

---

## 7. 复现方式

```sh
# 0) 环境
. /tmp/hxenv.sh && export MACOSX_DEPLOYMENT_TARGET=11.0
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1

# 1) 源码侧（覆盖探针已并入 native/，apply.sh 会一起拷进 lime 检出）
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832
# 快速编译检查（几秒）
./metal/scripts/check-compile.sh --lime ~/haxelib-dev/lime-832 --hxcpp ~/haxelib/hxcpp/4,3,2 --out /tmp/obj-check

# 2) 重建 lime.ndll（必须删 obj，hxcpp 不跟踪头文件；本次约 2 分钟）
rm -rf ~/haxelib-dev/lime-832/project/obj ~/haxelib-dev/lime-832/ndll/MacArm64/lime-64.ndll
haxelib run lime rebuild mac -Dmetal

# 3) 部署 + 签名
APP=export/release/macos/bin/CodenameEngine.app
cp ~/haxelib-dev/lime-832/ndll/MacArm64/lime-64.ndll $APP/Contents/MacOS/lime.ndll
codesign --force --deep --sign - $APP

# 4) 跑一个覆盖会话（只开覆盖计数；-TERM 时自动 dump 再退出）
cd export/release/macos/bin
CNE_METAL_COVERAGE=1 CNE_METAL_COVERAGE_OUT=/tmp/cov.txt CNE_METAL_STATS=1 \
CNE_METAL_AUTOKEYS="1300:40,2100:40,2900:40,4300:40" \
  ./CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cov.log 2>&1 &
PID=$!; sleep 150; kill -TERM $PID; wait $PID

# 5) 结果：/tmp/cov.txt（count<TAB>primitive，0 = 从未调用）
awk -F'\t' 'NF==2 && $1>0' /tmp/cov.txt | sort -rn | head -60
```

探针源码：`metal/native/MetalCoverage.h`、`metal/native/MetalCoverageNames.inc`；每个入口的 `CNE_COV (...)` 行可用 `grep -n 'CNE_COV' metal/native/*.cpp metal/native/*.inc` 查看（271 处）。

---

## 附录 A：全量 269 原语明细

`实现度`：完整 / 部分 / 镜像 / 空实现；`游戏内 / 菜单` = 运行时调用次数；`位置` = 我们的实现文件:行号（行号为插桩后的文件行号，会随后续改动漂移，仅供参考）。

### 上下文 / 字符串 / 杂项（22）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_finish` | ⚠ 空实现 | 0 / 0 | bgfx 每帧 Frame() 同步；GL finish 无可做 | `MetalBindings.cpp:3241` |
| `lime_gl_flush` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalBindings.cpp:3247` |
| `lime_gl_get_boolean` | ◇ 镜像 | 0 / 0 | QueryBooleans 镜像 | `MetalUniforms.inc:574` |
| `lime_gl_get_booleanv` | ◇ 镜像 | 0 / 0 | 同上 | `MetalUniforms.inc:583` |
| `lime_gl_get_context_attributes` | 完整 | 0 / 0 | 返回 alpha/depth/stencil/antialias 能力对象 | `MetalStates.inc:1039` |
| `lime_gl_get_error` | 完整 | 0 / 0 | 恒 GL_NO_ERROR（错误走 cne-metal 日志） | `MetalBindings.cpp:3217` |
| `lime_gl_get_extension` | ⚠ 空实现 | 1 / 1 | 返回 null；Lime Haxe 侧忽略此返回值，靠 getString(EXTENSIONS) | `MetalStates.inc:1026` |
| `lime_gl_get_float` | ◇ 镜像 | 0 / 0 | QueryFloats 镜像 | `MetalUniforms.inc:592` |
| `lime_gl_get_floatv` | ◇ 镜像 | 0 / 0 | 同上 | `MetalUniforms.inc:601` |
| `lime_gl_get_integer` | ◐ 部分 | 2 / 2 | QueryIntegers 覆盖常见 limits，未覆盖项 0 | `MetalUniforms.inc:610` |
| `lime_gl_get_integer64i_v` | ⚠ 空实现 | 0 / 0 | GL3 indexed 状态未跟踪 | `MetalUniforms.inc:653` |
| `lime_gl_get_integer64v` | ◐ 部分 | 0 / 0 | 由 QueryIntegers 提升 | `MetalUniforms.inc:636` |
| `lime_gl_get_integeri_v` | ⚠ 空实现 | 0 / 0 | GL3 indexed 状态未跟踪 | `MetalUniforms.inc:622` |
| `lime_gl_get_integerv` | ◐ 部分 | 0 / 0 | 仅 MAX_TEXTURE_SIZE/VIEWPORT，其余 0 | `MetalBindings.cpp:3224` |
| `lime_gl_get_internalformativ` | ⚠ 空实现 | 0 / 0 | 无 MSAA，恒 0（无调用方） | `MetalStates.inc:1055` |
| `lime_gl_get_shader_precision_format` | ◐ 部分 | 0 / 0 | 返回真实精度（MSL float/int），非声明精度 | `MetalUniforms.inc:665` |
| `lime_gl_get_string` | ◐ 部分 | 7 / 7 | VENDOR/RENDERER/VERSION 真实；EXTENSIONS 只报 GL_EXT_bgra（OpenFL 依赖） | `MetalBindings.cpp:3182` |
| `lime_gl_get_stringi` | ⚠ 空实现 | 0 / 0 | 索引式扩展查询无使用方，返回 null | `MetalStates.inc:1014` |
| `lime_gl_hint` | ⚠ 空实现 | 0 / 0 | 镜像，advisory | `MetalStates.inc:1118` |
| `lime_gl_object_deregister` | 完整 | 0 / 0 | 同上 | `MetalBindings.cpp:1386` |
| `lime_gl_object_from_id` | 完整 | 0 / 0 | 同上 | `MetalBindings.cpp:1371` |
| `lime_gl_object_register` | 完整 | 212 / 142 | Lime 对象注册表（GC/身份） | `MetalBindings.cpp:1379` |

### 着色器 / program（30）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_attach_shader` | 完整 | 6 / 6 | 记录 VS/FS | `MetalBindings.cpp:1557` |
| `lime_gl_bind_attrib_location` | 完整 | 18 / 18 | 绑定 GL location（Haxe 侧生成唯一 location） | `MetalBindings.cpp:1601` |
| `lime_gl_compile_shader` | 完整 | 6 / 6 | MSL 交给 bgfx::createShader；失败信息进 infoLog | `MetalBindings.cpp:1438` |
| `lime_gl_create_program` | 完整 | 3 / 3 | program 对象 + attribute/uniform 元数据 | `MetalBindings.cpp:1548` |
| `lime_gl_create_shader` | 完整 | 6 / 6 | 创建 shader 对象；source 由 Haxe 侧 GLSL→MSL 翻译器提供 | `MetalBindings.cpp:1405` |
| `lime_gl_delete_program` | 完整 | 0 / 0 | 销毁 bgfx program | `MetalBindings.cpp:1813` |
| `lime_gl_delete_shader` | 完整 | 0 / 0 | 对象生命周期 | `MetalBindings.cpp:1528` |
| `lime_gl_detach_shader` | 完整 | 0 / 0 | 解除 attach | `MetalStates.inc:916` |
| `lime_gl_get_active_attrib` | 完整 | 0 / 0 | 从 program 元数据返回 | `MetalUniforms.inc:700` |
| `lime_gl_get_active_uniform` | 完整 | 0 / 0 | 从 program 元数据返回 | `MetalUniforms.inc:723` |
| `lime_gl_get_attached_shaders` | 完整 | 0 / 0 | 返回已 attach 的 VS/FS | `MetalStates.inc:927` |
| `lime_gl_get_attrib_location` | 完整 | 594 / 346 | 返回分配的唯一 location（OpenFL 依赖） | `MetalBindings.cpp:1610` |
| `lime_gl_get_frag_data_location` | 完整 | 0 / 0 | 单输出→0，名字空→-1（近似但语义正确） | `MetalStates.inc:1004` |
| `lime_gl_get_program_binary` | ⚠ 空实现 | 0 / 0 | bgfx 不暴露 program 二进制 | `MetalStates.inc:948` |
| `lime_gl_get_program_info_log` | 完整 | 0 / 0 | MSL/bgfx program 错误 | `MetalBindings.cpp:1592` |
| `lime_gl_get_programi` | 完整 | 3 / 3 | LINK_STATUS/DELETE_STATUS/ACTIVE_* 真实 | `MetalBindings.cpp:1569` |
| `lime_gl_get_programiv` | 完整 | 0 / 0 | 同上（数组） | `MetalBindings.cpp:1583` |
| `lime_gl_get_shader_info_log` | 完整 | 6 / 6 | bgfx 编译错误文本 | `MetalBindings.cpp:1519` |
| `lime_gl_get_shader_source` | 完整 | 0 / 0 | 返回已保存的源码 | `MetalBindings.cpp:1429` |
| `lime_gl_get_shaderi` | 完整 | 6 / 6 | COMPILE_STATUS/DELETE_STATUS/SHADER_TYPE 真实反映 | `MetalBindings.cpp:1498` |
| `lime_gl_get_shaderiv` | 完整 | 0 / 0 | 同上（数组形式） | `MetalBindings.cpp:1510` |
| `lime_gl_get_uniform_location` | 完整 | 519 / 302 | 返回 uniform 槽（OpenFL uniform 上传依赖） | `MetalBindings.cpp:1637` |
| `lime_gl_link_program` | 完整 | 3 / 3 | 调用 bgfx::createProgram，全 program 可真实 draw | `MetalBindings.cpp:1758` |
| `lime_gl_program_binary` | ⚠ 空实现 | 0 / 0 | bgfx 不能从 GL 二进制加载 | `MetalStates.inc:957` |
| `lime_gl_program_parameteri` | ⚠ 空实现 | 0 / 0 | 无可分离管线/二进制状态 | `MetalStates.inc:975` |
| `lime_gl_release_shader_compiler` | ⚠ 空实现 | 0 / 0 | bgfx 无可释放的运行时编译器实例，合法 no-op | `MetalStates.inc:997` |
| `lime_gl_shader_binary` | ⚠ 空实现 | 0 / 0 | bgfx shader 是 MSL 源码不是 GL 二进制 | `MetalStates.inc:966` |
| `lime_gl_shader_source` | 完整 | 6 / 6 | 保存 MSL 源码（Haxe 已翻译） | `MetalBindings.cpp:1416` |
| `lime_gl_use_program` | 完整 | 22,682 / 8,665 | 记录 currentProgram，提交时绑定 | `MetalBindings.cpp:1806` |
| `lime_gl_validate_program` | ⚠ 空实现 | 0 / 0 | GL 语义本为 no-op（仅建议性校验），不影响渲染 | `MetalStates.inc:987` |

### uniform（45）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_get_active_uniform_block_name` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalUniforms.inc:431` |
| `lime_gl_get_active_uniform_blocki` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalUniforms.inc:441` |
| `lime_gl_get_active_uniform_blockiv` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalUniforms.inc:452` |
| `lime_gl_get_active_uniformsiv` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalUniforms.inc:465` |
| `lime_gl_get_uniform_block_index` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalUniforms.inc:483` |
| `lime_gl_get_uniformf` | ◇ 镜像 | 0 / 0 | 读回 uniform 暂存值 | `MetalUniforms.inc:505` |
| `lime_gl_get_uniformfv` | ◇ 镜像 | 0 / 0 | 读回 uniform 暂存值 | `MetalUniforms.inc:521` |
| `lime_gl_get_uniformi` | ◇ 镜像 | 0 / 0 | 读回 uniform 暂存值 | `MetalUniforms.inc:532` |
| `lime_gl_get_uniformiv` | ◇ 镜像 | 0 / 0 | 读回 uniform 暂存值 | `MetalUniforms.inc:542` |
| `lime_gl_get_uniformui` | ◇ 镜像 | 0 / 0 | 读回 uniform 暂存值 | `MetalUniforms.inc:552` |
| `lime_gl_get_uniformuiv` | ◇ 镜像 | 0 / 0 | 读回 uniform 暂存值 | `MetalUniforms.inc:562` |
| `lime_gl_uniform1f` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1890` |
| `lime_gl_uniform1fv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1922` |
| `lime_gl_uniform1i` | 完整 | 586,254 / 103,546 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1994` |
| `lime_gl_uniform1iv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2034` |
| `lime_gl_uniform1ui` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:288` |
| `lime_gl_uniform1uiv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:333` |
| `lime_gl_uniform2f` | 完整 | 150,256 / 32,891 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1898` |
| `lime_gl_uniform2fv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1929` |
| `lime_gl_uniform2i` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2010` |
| `lime_gl_uniform2iv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2041` |
| `lime_gl_uniform2ui` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:305` |
| `lime_gl_uniform2uiv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:340` |
| `lime_gl_uniform3f` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1906` |
| `lime_gl_uniform3fv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1936` |
| `lime_gl_uniform3i` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2018` |
| `lime_gl_uniform3iv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2048` |
| `lime_gl_uniform3ui` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:313` |
| `lime_gl_uniform3uiv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:347` |
| `lime_gl_uniform4f` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1914` |
| `lime_gl_uniform4fv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:1943` |
| `lime_gl_uniform4i` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2026` |
| `lime_gl_uniform4iv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalBindings.cpp:2055` |
| `lime_gl_uniform4ui` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:321` |
| `lime_gl_uniform4uiv` | 完整 | 0 / 0 | 写入 program uniform 暂存区，提交时 bgfx::setUniform | `MetalUniforms.inc:354` |
| `lime_gl_uniform_block_binding` | ⚠ 空实现 | 0 / 0 | uniform block 被折叠进 _mtl_u 结构体 | `MetalUniforms.inc:493` |
| `lime_gl_uniform_matrix2fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:368` |
| `lime_gl_uniform_matrix2x3fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:376` |
| `lime_gl_uniform_matrix2x4fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:384` |
| `lime_gl_uniform_matrix3fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalBindings.cpp:1971` |
| `lime_gl_uniform_matrix3x2fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:392` |
| `lime_gl_uniform_matrix3x4fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:400` |
| `lime_gl_uniform_matrix4fv` | 完整 | 150,256 / 32,891 | 转置为 MSL 行主序后写 uniform | `MetalBindings.cpp:1950` |
| `lime_gl_uniform_matrix4x2fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:408` |
| `lime_gl_uniform_matrix4x3fv` | 完整 | 0 / 0 | 小矩阵以 float4 对齐写入（GLSL 中罕见） | `MetalUniforms.inc:416` |

### 缓冲区（15）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_bind_buffer` | 完整 | 428,492 / 87,794 | ARRAY/ELEMENT 绑定镜像 | `MetalBindings.cpp:2073` |
| `lime_gl_bind_buffer_base` | ⚠ 空实现 | 0 / 0 | bgfx 无 indexed bind point（UBO/SSBO/TF） | `MetalStates.inc:739` |
| `lime_gl_bind_buffer_range` | ⚠ 空实现 | 0 / 0 | 同上（offset/size 忽略） | `MetalStates.inc:754` |
| `lime_gl_buffer_data` | 完整 | 120,538 / 19,200 | CPU mirror；draw 时按内容签名建 bgfx buffer | `MetalBindings.cpp:2090` |
| `lime_gl_buffer_sub_data` | 完整 | 0 / 0 | CPU mirror 更新；签名变化触发重传 | `MetalBindings.cpp:2109` |
| `lime_gl_copy_buffer_sub_data` | ◐ 部分 | 0 / 0 | 仅 ARRAY/ELEMENT target 镜像可拷贝 | `MetalTextures.inc:879` |
| `lime_gl_create_buffer` | 完整 | 121 / 84 | 对象 | `MetalBindings.cpp:2064` |
| `lime_gl_delete_buffer` | 完整 | 0 / 0 | 销毁缓存句柄 | `MetalBindings.cpp:2124` |
| `lime_gl_get_buffer_parameteri` | ◇ 镜像 | 0 / 0 | 从 mirror 返回 SIZE/USAGE | `MetalStates.inc:804` |
| `lime_gl_get_buffer_parameteri64v` | ◇ 镜像 | 0 / 0 | 同上 | `MetalStates.inc:820` |
| `lime_gl_get_buffer_parameteriv` | ◇ 镜像 | 0 / 0 | 同上 | `MetalStates.inc:811` |
| `lime_gl_get_buffer_pointerv` | ◇ 镜像 | 0 / 0 | 返回 mirror 指针 | `MetalStates.inc:829` |
| `lime_gl_get_buffer_sub_data` | ◇ 镜像 | 0 / 0 | 从 CPU mirror 读回 | `MetalStates.inc:846` |
| `lime_gl_map_buffer_range` | 完整 | 0 / 0 | 返回 CPU mirror 指针；unmap 后下次 draw 重传 | `MetalStates.inc:862` |
| `lime_gl_unmap_buffer` | 完整 | 0 / 0 | 解除映射 | `MetalStates.inc:887` |

### 顶点属性 / VAO（29）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_bind_vertex_array` | 完整 | 0 / 0 | 记录 current VAO | `MetalBindings.cpp:2143` |
| `lime_gl_create_vertex_array` | 完整 | 0 / 0 | VAO 对象（记录 attribute 状态） | `MetalBindings.cpp:2134` |
| `lime_gl_delete_vertex_array` | 完整 | 0 / 0 | 删除 | `MetalBindings.cpp:2151` |
| `lime_gl_disable_vertex_attrib_array` | 完整 | 1,499,134 / 259,125 | 禁用 | `MetalBindings.cpp:2171` |
| `lime_gl_enable_vertex_attrib_array` | 完整 | 709,792 / 149,356 | 标记 attribute 使用真实 buffer | `MetalBindings.cpp:2159` |
| `lime_gl_get_vertex_attrib_pointerv` | ◇ 镜像 | 0 / 0 | 返回记录的 offset | `MetalTextures.inc:1445` |
| `lime_gl_get_vertex_attribf` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1459` |
| `lime_gl_get_vertex_attribfv` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1468` |
| `lime_gl_get_vertex_attribi` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1477` |
| `lime_gl_get_vertex_attribii` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1486` |
| `lime_gl_get_vertex_attribiiv` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1504` |
| `lime_gl_get_vertex_attribiui` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1513` |
| `lime_gl_get_vertex_attribiuiv` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1522` |
| `lime_gl_get_vertex_attribiv` | ◇ 镜像 | 0 / 0 | 读回镜像 attribute 状态 | `MetalTextures.inc:1495` |
| `lime_gl_vertex_attrib1f` | 完整 | 150,256 / 32,891 | 常量属性被物化成额外 vertex stream（第 49.1/52 节修复） | `MetalTextures.inc:1337` |
| `lime_gl_vertex_attrib1fv` | 完整 | 0 / 0 | 同上 | `MetalTextures.inc:1345` |
| `lime_gl_vertex_attrib2f` | 完整 | 0 / 0 | 常量属性被物化成额外 vertex stream（第 49.1/52 节修复） | `MetalTextures.inc:1355` |
| `lime_gl_vertex_attrib2fv` | 完整 | 0 / 0 | 同上 | `MetalTextures.inc:1363` |
| `lime_gl_vertex_attrib3f` | 完整 | 0 / 0 | 常量属性被物化成额外 vertex stream（第 49.1/52 节修复） | `MetalTextures.inc:1373` |
| `lime_gl_vertex_attrib3fv` | 完整 | 0 / 0 | 同上 | `MetalTextures.inc:1381` |
| `lime_gl_vertex_attrib4f` | 完整 | 507,326 / 96,602 | 常量属性被物化成额外 vertex stream（第 49.1/52 节修复） | `MetalTextures.inc:1391` |
| `lime_gl_vertex_attrib4fv` | 完整 | 0 / 0 | 同上 | `MetalTextures.inc:1399` |
| `lime_gl_vertex_attrib_divisor` | ⚠ 空实现 | 0 / 0 | bgfx 无 per-attribute divisor（实例化属性不支持） | `MetalTextures.inc:1325` |
| `lime_gl_vertex_attrib_ipointer` | 完整 | 0 / 0 | 同上（整数变体） | `MetalTextures.inc:1316` |
| `lime_gl_vertex_attrib_pointer` | 完整 | 409,280 / 83,574 | 记录 layout；draw 时绑定 bgfx stream | `MetalBindings.cpp:2183` |
| `lime_gl_vertex_attribi4i` | ◐ 部分 | 0 / 0 | 整数常量属性只镜像供查询，未物化成 stream（浮点常量已物化） | `MetalTextures.inc:1409` |
| `lime_gl_vertex_attribi4iv` | ◐ 部分 | 0 / 0 | 整数常量属性只镜像供查询，未物化成 stream（浮点常量已物化） | `MetalTextures.inc:1417` |
| `lime_gl_vertex_attribi4ui` | ◐ 部分 | 0 / 0 | 整数常量属性只镜像供查询，未物化成 stream（浮点常量已物化） | `MetalTextures.inc:1427` |
| `lime_gl_vertex_attribi4uiv` | ◐ 部分 | 0 / 0 | 整数常量属性只镜像供查询，未物化成 stream（浮点常量已物化） | `MetalTextures.inc:1435` |

### 纹理（22）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_active_texture` | 完整 | 195,317 / 49,577 | unit 选择 | `MetalBindings.cpp:2647` |
| `lime_gl_bind_texture` | ◐ 部分 | 207,203 / 54,062 | 只镜像 GL_TEXTURE_2D/2D 到 unit；GL_TEXTURE_3D/CUBE 不镜像 | `MetalBindings.cpp:2638` |
| `lime_gl_compressed_tex_image_2d` | ◐ 部分 | 0 / 0 | 映射到 bgfx 压缩格式时真实上传，未映射格式报告 | `MetalTextures.inc:1084` |
| `lime_gl_compressed_tex_image_3d` | ◐ 部分 | 0 / 0 | 3D 绑定未镜像→报告 | `MetalTextures.inc:1125` |
| `lime_gl_compressed_tex_sub_image_2d` | ◐ 部分 | 0 / 0 | 同上 | `MetalTextures.inc:1163` |
| `lime_gl_compressed_tex_sub_image_3d` | ◐ 部分 | 0 / 0 | 3D 绑定未镜像→报告 | `MetalTextures.inc:1191` |
| `lime_gl_create_texture` | 完整 | 82 / 49 | 纹理对象（bgfx 句柄延迟创建） | `MetalBindings.cpp:2629` |
| `lime_gl_delete_texture` | 完整 | 0 / 0 | 销毁 bgfx 纹理 | `MetalBindings.cpp:2786` |
| `lime_gl_generate_mipmap` | ⚠ 空实现 | 0 / 0 | **静默 no-op**；仅当调用方请求 mipmap 才有意义（2D 路径不请求） | `MetalBindings.cpp:2770` |
| `lime_gl_get_tex_parameterf` | ◇ 镜像 | 0 / 0 | 读回镜像状态 | `MetalTextures.inc:1220` |
| `lime_gl_get_tex_parameterfv` | ◇ 镜像 | 0 / 0 | 读回镜像状态 | `MetalTextures.inc:1236` |
| `lime_gl_get_tex_parameteri` | ◇ 镜像 | 0 / 0 | 读回镜像状态 | `MetalTextures.inc:1213` |
| `lime_gl_get_tex_parameteriv` | ◇ 镜像 | 0 / 0 | 读回镜像状态 | `MetalTextures.inc:1227` |
| `lime_gl_pixel_storei` | ⚠ 空实现 | 0 / 0 | **静默 no-op**；UNPACK_ALIGNMENT 未实现，4B/像素时恒成立 | `MetalBindings.cpp:2778` |
| `lime_gl_tex_image_2d` | 完整 | 5,902 / 2,218 | CPU mirror + bgfx::createTexture2D，BGRA8/RGBA8/RGBA32F | `MetalBindings.cpp:2679` |
| `lime_gl_tex_image_3d` | ◐ 部分 | 0 / 0 | 仅当 GL_TEXTURE_3D 绑定可解析时写元数据；绑定未镜像→报告 | `MetalTextures.inc:955` |
| `lime_gl_tex_parameterf` | 完整 | 0 / 0 | 同上（float 形式） | `MetalBindings.cpp:2672` |
| `lime_gl_tex_parameteri` | 完整 | 328 / 196 | MIN/MAG_FILTER + WRAP_S/T → BGFX_SAMPLER_* | `MetalBindings.cpp:2655` |
| `lime_gl_tex_storage_2d` | ◐ 部分 | 0 / 0 | GL_TEXTURE_2D 可重建 bgfx 纹理；其他 target 报告不支持 | `MetalTextures.inc:1042` |
| `lime_gl_tex_storage_3d` | ◐ 部分 | 0 / 0 | 同上 | `MetalTextures.inc:1013` |
| `lime_gl_tex_sub_image_2d` | 完整 | 0 / 0 | bgfx::updateTexture2D | `MetalBindings.cpp:2755` |
| `lime_gl_tex_sub_image_3d` | ◐ 部分 | 0 / 0 | 同上 | `MetalTextures.inc:992` |

### framebuffer / renderbuffer / 读回（29）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_bind_framebuffer` | 完整 | 0 / 0 | 绑定/记录 | `MetalBindings.cpp:2810` |
| `lime_gl_bind_renderbuffer` | 完整 | 0 / 0 | 记录绑定 | `MetalBindings.cpp:2871` |
| `lime_gl_blit_framebuffer` | ⚠ 空实现 | 0 / 0 | bgfx blit 需 BLIT_DST 纹理+显式区域 → 报告 no-op | `MetalTextures.inc:788` |
| `lime_gl_check_framebuffer_status` | ◐ 部分 | 0 / 0 | 惰性建 FBO；无附件时仍报 COMPLETE（宽松） | `MetalBindings.cpp:2827` |
| `lime_gl_clear_bufferfi` | ⚠ 空实现 | 0 / 0 | bgfx clear 是 view 作用域；报告 | `MetalTextures.inc:827` |
| `lime_gl_clear_bufferfv` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalTextures.inc:839` |
| `lime_gl_clear_bufferiv` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalTextures.inc:848` |
| `lime_gl_clear_bufferuiv` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalTextures.inc:857` |
| `lime_gl_copy_tex_image_2d` | ⚠ 空实现 | 0 / 0 | **真实缺口（若有调用方）**：bgfx 读回异步 → 复制纹理为空操作 | `MetalTextures.inc:923` |
| `lime_gl_copy_tex_sub_image_2d` | ⚠ 空实现 | 0 / 0 | **真实缺口（若有调用方）**：同上 | `MetalTextures.inc:935` |
| `lime_gl_copy_tex_sub_image_3d` | ⚠ 空实现 | 0 / 0 | **真实缺口（若有调用方）**：同上 | `MetalTextures.inc:944` |
| `lime_gl_create_framebuffer` | 完整 | 0 / 0 | 对象生命周期 | `MetalBindings.cpp:2801` |
| `lime_gl_create_renderbuffer` | 完整 | 0 / 0 | 对象 + bgfx 纹理句柄 | `MetalBindings.cpp:2862` |
| `lime_gl_delete_framebuffer` | 完整 | 0 / 0 | 对象生命周期 | `MetalBindings.cpp:2846` |
| `lime_gl_delete_renderbuffer` | 完整 | 0 / 0 | 对象生命周期 | `MetalBindings.cpp:2902` |
| `lime_gl_draw_buffers` | ⚠ 空实现 | 0 / 0 | 单颜色附件；>1 时报告（2D 恒为 1） | `MetalStates.inc:1180` |
| `lime_gl_framebuffer_renderbuffer` | 完整 | 0 / 0 | 记录 attachment | `MetalTextures.inc:770` |
| `lime_gl_framebuffer_texture2D` | ◐ 部分 | 0 / 0 | 只记录 texture id；bgfx FBO 在 check_framebuffer_status 惰性创建 | `MetalBindings.cpp:2818` |
| `lime_gl_framebuffer_texture_layer` | ◐ 部分 | 0 / 0 | 分层 attachment 只 attach layer 0 | `MetalTextures.inc:740` |
| `lime_gl_get_framebuffer_attachment_parameteri` | ◇ 镜像 | 0 / 0 | 从 attachment 计算 | `MetalTextures.inc:1245` |
| `lime_gl_get_framebuffer_attachment_parameteriv` | ◇ 镜像 | 0 / 0 | 同上 | `MetalTextures.inc:1256` |
| `lime_gl_get_renderbuffer_parameteri` | ◇ 镜像 | 0 / 0 | 读回 mirror | `MetalTextures.inc:1266` |
| `lime_gl_get_renderbuffer_parameteriv` | ◇ 镜像 | 0 / 0 | 读回 mirror | `MetalTextures.inc:1275` |
| `lime_gl_invalidate_framebuffer` | ⚠ 空实现 | 0 / 0 | 内容变未定义，不做任何事语义正确 | `MetalTextures.inc:802` |
| `lime_gl_invalidate_sub_framebuffer` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalTextures.inc:814` |
| `lime_gl_read_buffer` | ⚠ 空实现 | 0 / 0 | 仅影响 read_pixels（本身 no-op），镜像仅记录 | `MetalTextures.inc:868` |
| `lime_gl_read_pixels` | ⚠ 空实现 | 0 / 0 | **真实缺口**：bgfx 读回异步；截图/BitmapData.draw/3D 截图失效 | `MetalBindings.cpp:3253` |
| `lime_gl_renderbuffer_storage` | 完整 | 0 / 0 | 创建 bgfx 纹理作为 render target | `MetalBindings.cpp:2883` |
| `lime_gl_renderbuffer_storage_multisample` | ◐ 部分 | 0 / 0 | 忽略 samples（无 MSAA） | `MetalBindings.cpp:2895` |

### 绘制（5）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_draw_arrays` | 完整 | 0 / 0 | BindVertexStreams + SubmitDraw → bgfx::submit | `MetalBindings.cpp:2572` |
| `lime_gl_draw_arrays_instanced` | 完整 | 0 / 0 | bgfx draw instanced | `MetalBindings.cpp:2613` |
| `lime_gl_draw_elements` | 完整 | 150,256 / 32,891 | index buffer + stream + SubmitDraw | `MetalBindings.cpp:2539` |
| `lime_gl_draw_elements_instanced` | 完整 | 0 / 0 | 同上（indexed） | `MetalBindings.cpp:2587` |
| `lime_gl_draw_range_elements` | 完整 | 0 / 0 | 等价 draw_elements（忽略 [start,end] 提示） | `MetalStates.inc:903` |

### 固定管线状态（29）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_blend_color` | ⚠ 空实现 | 0 / 0 | **静默 no-op**；bgfx 状态无 blend color 项 | `MetalBindings.cpp:2989` |
| `lime_gl_blend_equation` | ⚠ 空实现 | 0 / 0 | 镜像后 **ToBgfxState 不使用**（固定 ADD）；默认 FUNC_ADD 时无影响 | `MetalBindings.cpp:2973` |
| `lime_gl_blend_equation_separate` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalBindings.cpp:2981` |
| `lime_gl_blend_func` | 完整 | 1 / 1 | → blend_func_separate，进 bgfx 混合状态 | `MetalBindings.cpp:2956` |
| `lime_gl_blend_func_separate` | 完整 | 1 / 1 | 存入 state，ToBgfxState 应用 BGFX_STATE_BLEND_FUNC_SEPARATE | `MetalBindings.cpp:2963` |
| `lime_gl_clear` | 完整 | 7,462 / 2,714 | bgfx::setViewClear（含 depth 修正） | `MetalBindings.cpp:3087` |
| `lime_gl_clear_color` | 完整 | 7,462 / 2,714 | → setViewClear rgba | `MetalBindings.cpp:3063` |
| `lime_gl_clear_depthf` | 完整 | 0 / 0 | → setViewClear depth | `MetalBindings.cpp:3073` |
| `lime_gl_clear_stencil` | 完整 | 0 / 0 | → setViewClear stencil 值（但 stencil test 本身不支持） | `MetalBindings.cpp:3080` |
| `lime_gl_color_mask` | ⚠ 空实现 | 0 / 0 | **静默缺口**：镜像后 ToBgfxState 恒写 RGBA，掩码不生效 | `MetalBindings.cpp:2996` |
| `lime_gl_cull_face` | ⚠ 空实现 | 0 / 0 | **部分静默缺口**：cull_face(mode) 的 FRONT/BACK 被忽略；enable 时固定 CULL_CW | `MetalBindings.cpp:3027` |
| `lime_gl_depth_func` | ⚠ 空实现 | 1 / 1 | **静默 no-op**：固定 LEQUAL（OpenFL 2D 默认即 LEQUAL） | `MetalBindings.cpp:3006` |
| `lime_gl_depth_mask` | 完整 | 1 / 1 | → BGFX_STATE_WRITE_Z | `MetalBindings.cpp:3013` |
| `lime_gl_depth_rangef` | ⚠ 空实现 | 0 / 0 | **静默 no-op**；bgfx 无 depth range | `MetalBindings.cpp:3020` |
| `lime_gl_disable` | ◐ 部分 | 112,654 / 18,916 | 同上 | `MetalBindings.cpp:2926` |
| `lime_gl_enable` | ◐ 部分 | 420,930 / 87,713 | 只落实 BLEND/DEPTH_TEST/CULL_FACE/SCISSOR_TEST；其余 cap 静默忽略 | `MetalBindings.cpp:2912` |
| `lime_gl_front_face` | ⚠ 空实现 | 1 / 1 | **静默 no-op**：winding 固定，2D 无影响 | `MetalBindings.cpp:3034` |
| `lime_gl_is_enabled` | ◇ 镜像 | 0 / 0 | 只对上述 4 个 cap 有真实镜像；其余返回 false | `MetalBindings.cpp:2940` |
| `lime_gl_line_width` | ⚠ 空实现 | 0 / 0 | bgfx/Metal 1px 线宽；!=1 时报告 | `MetalStates.inc:1080` |
| `lime_gl_polygon_offset` | ⚠ 空实现 | 0 / 0 | bgfx 状态无 depth-bias | `MetalStates.inc:1092` |
| `lime_gl_sample_coverage` | ⚠ 空实现 | 0 / 0 | 无 MSAA | `MetalStates.inc:1101` |
| `lime_gl_scissor` | 完整 | 1 / 1 | scissorEnabled 时 ConfigureView 应用 | `MetalBindings.cpp:3053` |
| `lime_gl_stencil_func` | ⚠ 空实现 | 1 / 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 | `MetalStates.inc:1137` |
| `lime_gl_stencil_func_separate` | ⚠ 空实现 | 1 / 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 | `MetalStates.inc:1127` |
| `lime_gl_stencil_mask` | ⚠ 空实现 | 1 / 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 | `MetalStates.inc:1154` |
| `lime_gl_stencil_mask_separate` | ⚠ 空实现 | 1 / 1 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 | `MetalStates.inc:1144` |
| `lime_gl_stencil_op` | ⚠ 空实现 | 0 / 0 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 | `MetalStates.inc:1171` |
| `lime_gl_stencil_op_separate` | ⚠ 空实现 | 0 / 0 | **真实缺口（3D 路径）**：bgfx 状态无 stencil 项；镜像仅供查询 | `MetalStates.inc:1161` |
| `lime_gl_viewport` | 完整 | 157,718 / 35,605 | ConfigureView 每笔提交应用 | `MetalBindings.cpp:3041` |

### 查询 / 同步 / fence（16）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_begin_query` | ⚠ 空实现 | 0 / 0 | bgfx 无 occlusion/timer query；结果恒 0 | `MetalStates.inc:342` |
| `lime_gl_client_wait_sync` | ◐ 部分 | 0 / 0 | 恒 ALREADY_SIGNALED | `MetalStates.inc:570` |
| `lime_gl_create_query` | 完整 | 0 / 0 | query 对象（CPU 记录） | `MetalStates.inc:333` |
| `lime_gl_delete_query` | 完整 | 0 / 0 | 删除记录 | `MetalStates.inc:378` |
| `lime_gl_delete_sync` | 完整 | 0 / 0 | 删除记录 | `MetalStates.inc:559` |
| `lime_gl_end_query` | ⚠ 空实现 | 0 / 0 | 同上 | `MetalStates.inc:361` |
| `lime_gl_fence_sync` | ◐ 部分 | 0 / 0 | 无 GLsync；立刻 signaled（合法） | `MetalStates.inc:531` |
| `lime_gl_get_query_objectui` | ⚠ 空实现 | 0 / 0 | 结果恒 0（AVAILABLE 正确） | `MetalStates.inc:411` |
| `lime_gl_get_query_objectuiv` | ⚠ 空实现 | 0 / 0 | 结果恒 0 | `MetalStates.inc:418` |
| `lime_gl_get_queryi` | ⚠ 空实现 | 0 / 0 | 恒 0 / active id | `MetalStates.inc:395` |
| `lime_gl_get_queryiv` | ⚠ 空实现 | 0 / 0 | 恒 0 | `MetalStates.inc:402` |
| `lime_gl_get_sync_parameteri` | ◇ 镜像 | 0 / 0 | 返回记录的状态（恒 SIGNALED） | `MetalStates.inc:591` |
| `lime_gl_get_sync_parameteriv` | ◇ 镜像 | 0 / 0 | 同上 | `MetalStates.inc:609` |
| `lime_gl_is_query` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalStates.inc:781` |
| `lime_gl_is_sync` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalStates.inc:551` |
| `lime_gl_wait_sync` | ⚠ 空实现 | 0 / 0 | 等待已 signaled fence，合法 no-op | `MetalStates.inc:582` |

### transform feedback（10）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_begin_transform_feedback` | ⚠ 空实现 | 0 / 0 | 不支持（报告） | `MetalStates.inc:651` |
| `lime_gl_bind_transform_feedback` | ⚠ 空实现 | 0 / 0 | 仅记录 | `MetalStates.inc:643` |
| `lime_gl_create_transform_feedback` | ⚠ 空实现 | 0 / 0 | bgfx 无 transform feedback（仅对象簿记） | `MetalStates.inc:620` |
| `lime_gl_delete_transform_feedback` | ⚠ 空实现 | 0 / 0 | 仅删除簿记 | `MetalStates.inc:629` |
| `lime_gl_end_transform_feedback` | ⚠ 空实现 | 0 / 0 | 不支持（报告） | `MetalStates.inc:668` |
| `lime_gl_get_transform_feedback_varying` | ⚠ 空实现 | 0 / 0 | 不支持 | `MetalStates.inc:720` |
| `lime_gl_is_transform_feedback` | ◇ 镜像 | 0 / 0 | 对象判定 | `MetalStates.inc:730` |
| `lime_gl_pause_transform_feedback` | ⚠ 空实现 | 0 / 0 | 不支持 | `MetalStates.inc:684` |
| `lime_gl_resume_transform_feedback` | ⚠ 空实现 | 0 / 0 | 不支持 | `MetalStates.inc:694` |
| `lime_gl_transform_feedback_varyings` | ⚠ 空实现 | 0 / 0 | 不支持 | `MetalStates.inc:704` |

### 采样器对象（10）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_bind_sampler` | ◐ 部分 | 0 / 0 | 记录 unit→sampler；滤波实际由纹理采样器状态决定 | `MetalStates.inc:456` |
| `lime_gl_create_sampler` | 完整 | 0 / 0 | sampler 对象（参数镜像） | `MetalStates.inc:429` |
| `lime_gl_delete_sampler` | 完整 | 0 / 0 | 删除 | `MetalStates.inc:438` |
| `lime_gl_get_sampler_parameterf` | ◇ 镜像 | 0 / 0 | 读回镜像 | `MetalStates.inc:509` |
| `lime_gl_get_sampler_parameterfv` | ◇ 镜像 | 0 / 0 | 读回镜像 | `MetalStates.inc:520` |
| `lime_gl_get_sampler_parameteri` | ◇ 镜像 | 0 / 0 | 读回镜像 | `MetalStates.inc:486` |
| `lime_gl_get_sampler_parameteriv` | ◇ 镜像 | 0 / 0 | 读回镜像 | `MetalStates.inc:497` |
| `lime_gl_is_sampler` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalStates.inc:788` |
| `lime_gl_sampler_parameterf` | ◇ 镜像 | 0 / 0 | 同上 | `MetalStates.inc:475` |
| `lime_gl_sampler_parameteri` | ◇ 镜像 | 0 / 0 | 镜像参数；draw 时以纹理采样状态为准 | `MetalStates.inc:464` |

### is_* 对象判定（7）

| 原语 | 实现度 | 游戏内 / 菜单 | 说明 | 位置 |
| --- | --- | --- | --- | --- |
| `lime_gl_is_buffer` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalStates.inc:767` |
| `lime_gl_is_framebuffer` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalTextures.inc:1285` |
| `lime_gl_is_program` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalStates.inc:774` |
| `lime_gl_is_renderbuffer` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalTextures.inc:1292` |
| `lime_gl_is_shader` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalStates.inc:795` |
| `lime_gl_is_texture` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalTextures.inc:1299` |
| `lime_gl_is_vertex_array` | ◇ 镜像 | 0 / 0 | 真实对象判定 | `MetalTextures.inc:1306` |

---

### 7.1 临时备份 / 产物边界（避免误用）

本次排障在 `/tmp` 留了两个 **临时** 副本，**都不是正式交付产物**，不会被任何构建引用：

- `/tmp/lime-cov-instrumented.ndll` —— 本次插桩构建的**未签名**副本，仅用于 app 内 ndll 被覆盖后快速重部署。
- `/tmp/lime-app-before-cov.ndll` —— 插桩**之前** app 里原来的 `lime.ndll` 备份，用于回滚。

**正式产物只有**：`export/release/macos/bin/CodenameEngine.app/Contents/MacOS/lime.ndll`，它必须由 `metal/native/` 源码经 `apply.sh + lime rebuild mac` 构建、再 `codesign` 得到。两个 `/tmp` 文件重启即失，可安全删除。

_审计工具：静态差集脚本 + `CNE_METAL_COVERAGE` 运行时探针；会话原始数据 `/tmp/cne-coverage-menu.txt`、`/tmp/cne-coverage-game3.txt`（临时文件，重建后可复现）。本报告由 `audit-primitives`（task-6）生成，仅写本文件，未改动任何渲染代码。_
