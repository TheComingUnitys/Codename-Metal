# task-6 渲染状态排查报告（agent-states）

范围：`CodenameEngine-1.0.1/metal/native/MetalStates.inc`（本次 **0 行改动**，理由见 §6）
报告：本文件。编译验证：`check-compile.sh` 通过（§8）。

---

## 0. 结论（TL;DR）

1. **两个实测 state 值逐位解码后没有任何多余/非法位**（leftover = 0，见 §1）。两者都：
   - 低半字节 = `0xf` → `BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A` ✅
   - `BGFX_STATE_CULL_MASK`（bit 36-37）= 0 → **剔除是关闭的**
   - 混合字段是两个合法的 `BGFX_STATE_BLEND_FUNC_SEPARATE(...)`，blend equation = ADD ✅
2. 因此 **按这两个采样值，渲染状态不可能把全部几何体丢弃**：剔除不是原因（关闭），写入掩码不是原因（恒为 RGBA），混合不是原因（ONE/ZERO 直写 与 ONE/INV_SRC_ALPHA 预乘，均正确）。
3. 唯一可疑的置位 `BGFX_STATE_DEPTH_TEST_LEQUAL`（`0x20`）**不是映射错误**：bgfx Metal 把 LEQUAL 直译成 `MTLCompareFunctionLessEqual`（无 reverse-Z，`renderer_mtl.cpp:191-201`），而 `state.depthTest=true` 是 **OpenFL 自己打开**的（`Stage` → `configureBackBuffer(..., enableDepthAndStencil=true, ...)` → `Context3D.__flushGLFramebuffer()` → `gl.enable(GL_DEPTH_TEST)`）。
4. 但顺着深度这条线发现一个**真实缺陷**（修复点不在我的写入范围）：
   `lime_gl_clear()` 把 GL 的 clear mask 原样转成 `bgfx::setViewClear()`，而该方法语义是**替换**；OpenFL 2D 只清 COLOR → view 0 的 clear flags 被覆盖，`MetalContext::Create` 里装好的 `BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL` 永久失效 → **深度缓冲在整个进程生命周期内从未被清过**。只要片元 z 不为 0（属性/矩阵异常，或以后引入 GL→Metal 的 `z*0.5+0.5` 转换），`z_frag <= depth_buf` 就会全假 → 全屏几何体消失、颜色清屏照常可见。精确补丁见 §5（2 行，在 `MetalBindings.cpp`）。
5. 另外发现 4 个**潜伏缺陷**（不影响当前"全不可见"的现象，但会在别的路径上出错）：`glColorMask` 被忽略、blend equation 被忽略、`glDepthFunc` 被忽略、`glCullFace/glFrontFace` 被忽略且剔除方向写死为 `BGFX_STATE_CULL_CW`。
6. 采样值来源提醒：两个值来自 **2026-10-14** 的日志（`/private/tmp/{CV,DPI,LAY}.log` 的 `[cne-dbg]` 行；DPI/LAY 中 `rect=(0,0,2560,1440)`），即 CAMetalLayer 修复之前的构建。产生它们的 `ToBgfxState()`（`MetalBindings.cpp:269-298`，今天仍是同一段代码）没有变化，所以解码/结论仍适用。

---

## 1. 两个 state 值的逐位解码

位定义来源：`bgfx-master/include/bgfx/defines.h:24-135`（`BGFX_STATE_*`），`BGFX_STATE_BLEND_FUNC_SEPARATE` 见 `defines.h:611-615`。

### state A = `0x400121202f`

| 位（bit） | 掩码 | 本值 | 常量 |
| --- | --- | --- | --- |
| 0-3 | `0xf` | `0xf` | `BGFX_STATE_WRITE_R\|WRITE_G\|WRITE_B\|WRITE_A` |
| 4-7 | `0xf0` | `0x20` | `BGFX_STATE_DEPTH_TEST_LEQUAL` |
| 12-15 | `0xf000` | `0x2` | `BGFX_STATE_BLEND_ONE`（srcRGB） |
| 16-19 | `0xf0000` | `0x1` | `BGFX_STATE_BLEND_ZERO`（dstRGB） |
| 20-23 | `0xf00000` | `0x2` | `BGFX_STATE_BLEND_ONE`（srcA） |
| 24-27 | `0xf000000` | `0x1` | `BGFX_STATE_BLEND_ZERO`（dstA） |
| 28-31 | `0xf0000000` | `0` | `BLEND_EQUATION_ADD`（默认） |
| 38 | `0x4000000000` | `1` | `BGFX_STATE_WRITE_Z` |
| 36-37 | `0x3000000000` | `0` | **无剔除** |
| 其余 | — | `0` | 无 `FRONT_CCW` / `PT_*` / `ALPHA_REF` / `MSAA` / `BLEND_INDEPENDENT` / `ALPHA_TO_COVERAGE` |

= `WRITE_RGBA | DEPTH_TEST_LEQUAL | BLEND_FUNC_SEPARATE(ONE,ZERO,ONE,ZERO)`（=`0x1212000`）`| WRITE_Z`
**leftover = `0x0`** → 没有无法解释的位。
语义：不混合直写 + 深度测试开 + 深度写开。

### state B = `0x626202f`

| 位（bit） | 掩码 | 本值 | 常量 |
| --- | --- | --- | --- |
| 0-3 | `0xf` | `0xf` | `WRITE_R\|G\|B\|A` |
| 4-7 | `0xf0` | `0x20` | `BGFX_STATE_DEPTH_TEST_LEQUAL` |
| 12-15 | `0xf000` | `0x2` | `BLEND_ONE`（srcRGB） |
| 16-19 | `0xf0000` | `0x6` | `BLEND_INV_SRC_ALPHA`（dstRGB） |
| 20-23 | `0xf00000` | `0x2` | `BLEND_ONE`（srcA） |
| 24-27 | `0xf000000` | `0x6` | `BLEND_INV_SRC_ALPHA`（dstA） |
| 其余 | — | `0` | 无 `WRITE_Z`、无剔除、无其它 |

= `WRITE_RGBA | DEPTH_TEST_LEQUAL | BLEND_FUNC_SEPARATE(ONE,INV_SRC_ALPHA,ONE,INV_SRC_ALPHA)`（=`0x6262000`）
**leftover = `0x0`**。
语义：OpenFL 标准**预乘 alpha** 混合 + 深度测试开、深度写关（`glDepthMask(false)` 已被调用过）。

> 核对方式：手工 + python 逐字段核算（掩码交集），与 `defines.h` 的宏展开逐条对照，两个值都恰好只有上述 3 个来源（写入掩码 / DEPTH_TEST / BLEND_FUNC），不存在"不该出现的位"。

---

## 2. 逐项核实（对照 task-6 的 5 条怀疑）

### 2.1 三角面朝向 / 剔除（怀疑 #1）→ **排除**

* 两个采样 state 的 `BGFX_STATE_CULL_MASK` 都是 0。bgfx Metal 的映射：`s_cullMode[] = {None, CullModeFront, CullModeBack, None}`（`renderer_mtl.cpp:156-161`），`cullIndex = (state & CULL_MASK) >> 36`（`renderer_mtl.cpp:7020-7022`）→ 本情形是 `MTLCullModeNone`。**没有任何三角形被剔除。**
* OpenFL 2D 每次 `OpenGLRenderer.__render()` 都先 `__context3D.setCulling(NONE)`（`.haxelib/openfl/git/src/openfl/display/OpenGLRenderer.hx:757` 附近），所以剔除本来就是关的。
* 结论：**Y 翻转 / 绕序 / CULL 方向无论对错，都不可能造成"全部不可见"**（因为剔除没开）。
* 但映射本身有潜伏缺陷：`ToBgfxState()` 写死 `BGFX_STATE_CULL_CW`（`MetalBindings.cpp:292-295`），而 `lime_gl_cull_face()` / `lime_gl_front_face()` 直接 `(void)mode; (void)face;`（`MetalBindings.cpp:1984-1994`）→ GL 的剔除方向与正面绕序**完全传不下来**。bgfx 的实际语义（Metal 后端）：
  * `BGFX_STATE_CULL_CW` → `MTLCullModeFront`，`BGFX_STATE_CULL_CCW` → `MTLCullModeBack`，`BGFX_STATE_FRONT_CCW` → `WindingCounterClockwise`（`renderer_mtl.cpp:156-161, 7010-7022`）；即 `CULL_CW` 是"剔除 bgfx 约定下的正面"。
  * 要严格复刻 GL 默认（front=CCW、`glCullFace(GL_BACK)`）必须把 `glCullFace/glFrontFace` 记进 state 并据此选 `CULL_CW/CULL_CCW (+ FRONT_CCW)`。
  * **建议**：在画面可见性跑通前不要改这一行（避免同时引入两个变量）；修复方式见 §5 补丁 3。

### 2.2 深度测试（怀疑 #2）→ 位是真的、来源不是 bug；**但"深度从未清"是真实缺陷**

* `depthTest=false` 时确实**不设** `BGFX_STATE_DEPTH_TEST_*`：`MetalBindings.cpp:273-276`（`if (state.depthTest) state |= BGFX_STATE_DEPTH_TEST_LEQUAL;`）；`MetalContext.h:159` 默认 `depthTest = false`。✅ 映射本身正确。
* 两个采样都带 `0x20`，说明运行期 `state.depthTest == true`。来源已定位（**不是我们的 bug**）：
  1. `Stage` 建后台缓冲时第 4 个实参（`enableDepthAndStencil`）写死 `true`：`.haxelib/openfl/git/src/openfl/display/Stage.hx:1382`、`3656`（`context3D.configureBackBuffer(windowWidth, windowHeight, 0, true, true, true)`）；签名见 `Context3D.hx:593`。
  2. `Context3D.__flushGLFramebuffer()` 无条件 `__setGLDepthTest(__state.backBufferEnableDepthAndStencil)`：`Context3D.hx:2232`。
  3. `__setGLDepthTest(true)` → `gl.enable(gl.DEPTH_TEST)`：`Context3D.hx:2664-2676`。
  4. `OpenGLRenderer` 里的 `setDepthTest(false, ALWAYS)`（`OpenGLRenderer.hx:757`）只写 `__state.depthMask`（只影响 `gl.depthMask`），**不会关掉 GL_DEPTH_TEST**。
* **真缺陷（严重）**：`lime_gl_clear(mask)`（`MetalBindings.cpp:2037-2073`）把 GL mask 直译：
  ```cpp
  if (mask & GL_COLOR_BUFFER_BIT)   flags |= BGFX_CLEAR_COLOR;
  if (mask & GL_DEPTH_BUFFER_BIT)   flags |= BGFX_CLEAR_DEPTH;
  if (mask & GL_STENCIL_BUFFER_BIT) flags |= BGFX_CLEAR_STENCIL;
  ...
  bgfx::setViewClear (view, flags, rgba, state.clearDepth, (uint8_t)state.clearStencil);
  ```
  `bgfx::setViewClear()` 是**替换**该 view 的 clear 状态。OpenFL 2D 的清屏只带 `Context3DClearMask.COLOR`（`OpenGLRenderer.__clear()`，`OpenGLRenderer.hx:455-466`）→ view 0 的 flags 从 `MetalContext::Create` 安装的 `BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH|BGFX_CLEAR_STENCIL`（`MetalContext.cpp:531`）变成**纯 COLOR**，而且 OpenFL 的清屏在任何绘制之前 —— 于是 bgfx 交换链的深度附件（`m_backBufferDepth`，`renderer_mtl.cpp:5319-5350`）**从进程启动起就没有被清过一次**，`depthAttachment->setClearDepth(clr.m_depth)`（`renderer_mtl.cpp:6606`）永远不会执行。
* 严重性评估（诚实版，含实测证据）：
  * 当前 GLSLToMSL 把 `gl_Position` 原样输出，不做 z 重映射（`GLSLToMSL.hx` 中 `gl_Position` → `xlatOut.position`；`/tmp/cne-shader-*.metal` 实证：`xlatOut.position = _mtl_u.openfl_Matrix * xlatIn.a_bitangent;`）。
  * OpenFL 2D 投影是 `createOrtho(..., -1000, 1000)`（`OpenGLRenderer.hx:971-972`），矩阵 z 行为 `(0, 0, -1/1000, 0)`、w 行为 `(0,0,0,1)`；实测顶点流布局里位置属性是 **`num=2`（float2，stride=8/12）**：`[cne-dbg] stream 0 stride=8 ... glLoc=0->attrib#0 num=2 type=0x1406 off=0`（`/private/tmp/LAY.log`、`DPI.log`）。
  * 于是（在 Metal 按规范给缺失分量补 `(0,0,0,1)` 的前提下）顶点进入着色器时 z=0、w=1 → clip z = 0 → **片元 depth = 0.0（近平面）** → `LEQUAL(0 ≤ 任何已存值)` 恒真 → **当前深度测试并不会丢弃片元**（这也是我把"当前黑屏根因是否为深度"评成中低的原因）。
  * 但这条链极脆，有两个岔口：
    1. 顶点流 `num=2` 而 MSL 的 `stage_in` 声明 `float4 a_bitangent`（`/tmp/cne-shader-0.metal`）—— 一旦 Metal 的补位不是 `(z=0,w=1)`（例如按声明宽度越界读下一条顶点），z/w 立刻变成 `(x1,y1)` 之类的垃圾，`z_frag<=depth_buf` 可能全假 + 近平面裁剪，两者都表现为"全屏几何体消失"。
    2. 以后若为对齐 Metal 约定把 GL NDC z 做 `z*0.5+0.5`（bgfx 自家 shaderc 对 Metal 就做这个），片元 depth 变成 0.5，此时**未清深度缓冲**（内容常为 0.0）会让 `0.5 <= 0.0` 全假 → 全屏几何体消失、而颜色清屏照常可见（与当前现象完全一致）。
  * 且**无法自愈**：没通过深度测试的片元不会写深度（state B 连 `WRITE_Z` 都没有），深度缓冲会永久保留初始垃圾值。
* **结论：这一处必须修**（无论它是否当前黑屏的根因）；顺便建议核实"位置流 num=2 / MSL 声明 float4"的补位行为（属属性/布局工作流）。

### 2.3 写入掩码（怀疑 #3）→ **排除**

* `ToBgfxState()` 第一行就是 `BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A`（`MetalBindings.cpp:271`），无任何分支；两个采样低半字节都是 `0xf`。✅
* 反向缺陷：`state.colorWrite[4]`（`MetalContext.h:163`，由 `lime_gl_color_mask` 记录，`MetalBindings.cpp:1957-1964`）**从未被折进 state**，所以 `glColorMask(false,false,false,false)` 之类被忽略。这个方向只会"多写"，不会造成不可见。

### 2.4 混合（怀疑 #4）→ **排除**

* `blendEnabled=false` → 不设任何 blend 位 → bgfx 关闭混合 → 源覆盖目标（等价 GL）。✅
* 默认 `blendSrcRGB=GL_ONE(1) / blendDstRGB=GL_ZERO(0)`（`MetalContext.h:166-169`）→ `BGFX_STATE_BLEND_FUNC_SEPARATE(ONE,ZERO,ONE,ZERO)`（= `0x1212000`），正是实测 state A。✅
* `ToBgfxBlend()` 对常用 GL factor 的映射正确（0→ZERO、1→ONE、0x0302→SRC_ALPHA、0x0303→INV_SRC_ALPHA、0x0300/0x0301/0x0304..0x0307 都在表里，`MetalBindings.cpp:300-316`）。实测 state B 的 `(ONE, INV_SRC_ALPHA)` 就是 OpenFL 标准预乘 alpha。✅
* 潜伏缺陷：
  1. `state.blendEqRGB/blendEqAlpha`（`MetalContext.h:169-170`，`MetalBindings.cpp:1937-1949` 记录）**从未折进 state** → `BlendMode.SUBTRACT`（GL_FUNC_REVERSE_SUBTRACT）会退化成 ADD，MIN/MAX 也失效。
  2. `ToBgfxBlend()` 对表外 factor 一律回落 `BGFX_STATE_BLEND_ONE`（`MetalBindings.cpp:314`），会让未知 factor 变成"加法偏亮"。
  3. 这两条都不会造成"不可见"。
* ⚠️ 一个**重要的旁路提示**：B 型混合 `(ONE, INV_SRC_ALPHA)` 下，若纹理采样返回 `(0,0,0,0)`（纹理/采样器绑定错误，或 `openfl_Alpha=0`、颜色变换 uniform 为 0），输出恒等于 dst → **看起来就是"没画"**。这与"state 把几何体丢了"在现象上无法区分。建议用 §7 的判别实验分开。

### 2.5 state 数值 vs bgfx 位定义 → **无多余位**

见 §1（leftover = 0）。另外确认 `bgfx::setState(uint64_t)` 与这里的 `long`（macOS/arm64 上 64 位）宽度一致，`0x400121202f` 不会截断。

---

## 3. 与"可见性"无关但已核对的项目（免得后人重查）

| 项 | 结论 |
| --- | --- |
| `lime_gl_depth_mask` | 正确写入 `state.depthWrite`（`MetalBindings.cpp:1973-1976`）→ `BGFX_STATE_WRITE_Z` |
| `lime_gl_depth_func` | **被忽略**（`(void)func`，`MetalBindings.cpp:1967-1971`），永远 LEQUAL（GL 默认 LESS，2D 无影响） |
| `lime_gl_depth_rangef` | no-op（bgfx 无 depth range，合理） |
| `lime_gl_clear_depth` / `clear_depthf` | 只写 `state.clearDepth`，被 `lime_gl_clear` 使用，正确（`MetalStates.inc:1001-1007`） |
| stencil | `lime_gl_enable/disable` 的 switch 里**没有** `GL_STENCIL_TEST`（`MetalBindings.cpp:40-60` 枚举里也没有），`ToBgfxState()` 不折 stencil 位 → bgfx 侧 stencil 恒关（方向安全：不会把几何体丢掉）；`MetalStates.inc` 只做镜像并 `ReportUnimplemented` |
| `lime_gl_get_context_attributes` | 返回 `depth=true, stencil=true`（`MetalStates.inc:972-984`）；**不是** `depthTest` 的来源（OpenFL 用的是 Haxe 侧 `windowAttributes.context`，且 `configureBackBuffer` 4 个实参写死 true），本文件无需改 |
| `bgfx::setState` 调用点 | `SubmitDraw()` → `MetalBindings.cpp:1546`（每次 submit 都重设，正确） |

---

## 4. 验证方法（本次实际执行的）

* 位解码：python 逐字段掩码核算 + 与 `bgfx-master/include/bgfx/defines.h` 手工对照（§1）。
* 静态链路：`bgfx-master/src/renderer_mtl.cpp` 的 compare/cull/hasDepth/clearDepth 实现；OpenFL(`.haxelib/openfl/git`)/lime(`~/haxelib-dev/lime-832`) 的清屏与深度测试调用链；本仓 `MetalBindings.cpp` / `MetalContext.cpp` / `MetalContext.h`。
* 编译：见 §8，通过。

---

## 5. 建议补丁（位于 `MetalBindings.cpp`，**不在本 agent 写入范围，未应用**）

### 补丁 1（首选，2 行）——让深度缓冲每帧被清

```cpp
// MetalBindings.cpp, lime_gl_clear()，约 2046-2053
 		if (mask & GL_DEPTH_BUFFER_BIT)
 		{
 			flags |= BGFX_CLEAR_DEPTH;
 		}
+		// bgfx::setViewClear() REPLACES the view's clear state.  OpenFL only ever
+		// clears GL_COLOR_BUFFER_BIT, so a literal translation silently drops the
+		// BGFX_CLEAR_DEPTH that MetalContext::Create installed, and the swap-chain
+		// depth texture is never cleared again.  While GL_DEPTH_TEST is on (OpenFL
+		// enables it via configureBackBuffer(..., enableDepthAndStencil=true, ...))
+		// every fragment is then tested against undefined depth.
+		if (MetalContext::state.depthTest || MetalContext::state.depthWrite)
+		{
+			flags |= BGFX_CLEAR_DEPTH;
+		}
```

### 补丁 2（备选，若判定 2D 根本不需要深度）——把深度测试降级

`ToBgfxState()`（`MetalBindings.cpp:269-298`）：把 `BGFX_STATE_DEPTH_TEST_LEQUAL` 换成 `BGFX_STATE_DEPTH_TEST_ALWAYS`，并且只在确有 3D 需求时保留 `BGFX_STATE_WRITE_Z`。
**补丁 1 与补丁 2 不要同时应用**（否则会掩盖 3D 路径的回归）。

### 补丁 3（潜伏，可延后）——把被忽略的 GL 状态补齐

* `lime_gl_cull_face(mode)` / `lime_gl_front_face(face)` 记进 `MetalContext::state`（需要 `MetalContext.h` 加字段），`ToBgfxState()` 据此在 `CULL_CW/CULL_CCW` 与 `FRONT_CCW` 之间选择；不要写死 `CULL_CW`。
* `lime_gl_depth_func(func)`：把 func 映射到 `BGFX_STATE_DEPTH_TEST_*`（目前永远 LEQUAL）。
* `lime_gl_color_mask(...)`：`ToBgfxState()` 按 `state.colorWrite[4]` 组 `BGFX_STATE_WRITE_R/G/B/A`（目前恒 RGB|A）。
* `lime_gl_blend_equation*`：把 `blendEqRGB/blendEqAlpha` 映射到 `BGFX_STATE_BLEND_EQUATION_SEPARATE(...)`（目前恒 ADD）。

---

## 6. 为什么 `MetalStates.inc` 改动 0 行

* 会造成/影响绘制的状态代码**全在 `MetalBindings.cpp`**：`ToBgfxState()`（269-298）、`lime_gl_enable/disable/is_enabled`（1881-1918）、`blend_func/_separate/equation*`（1922-1949）、`color_mask`（1957-1964）、`depth_func`（1967-1971）、`depth_mask`（1973-1976）、`cull_face/front_face`（1984-1994）、`clear`（2037-2073）。`MetalStates.inc` 里**没有任何一处**参与 draw state 的构建（只有 `lime_gl_clear_depth` 写 `clearDepth`、stencil 镜像、query/sampler/sync/TF 等）。
* `MetalStates.inc` 在 `MetalBindings.cpp` **第 2158 行**被 `#include`，位置在所有上述定义**之后**（`MetalBindings.cpp` 末尾的 hand-written extras 区），因此它只能**新增**符号，无法覆盖 `ToBgfxState()` / `lime_gl_clear()`，也无 per-frame 回调可以挂钩。
* 因此在我被限定的写入范围内**不存在需要修的真 bug**；强行改动只会引入无调用者的死代码。请 Lead 把 §5 补丁指派给 `MetalBindings.cpp` 的写入者，或扩展我的写入范围后由我执行（我可以立刻改并重新跑 `check-compile.sh`）。

---

## 7. 建议的运行时判别实验（我按约束没有运行游戏）

按判别力排序：

1. **应用补丁 1 后跑一次**：若几何体立刻出现 → 根因确认是"深度测试开 + 深度从未清"。
2. **变色判别**：把片元着色器临时改成恒定输出（例如 `xlatOut.color = float4(0,0,1,1);` 或直接 `return` 固定色）。
   * 出现蓝色块 → 状态/几何/顶点变换/属性都对，问题在**纹理采样或颜色变换**（下一步查 sampler 绑定与 `openfl_Alpha` / `openfl_ColorMultiplier`）。
   * 仍全黑 → 回到 §5 补丁 3 与矩阵/属性方向。
3. **绕开 `ToBgfxState()`**：临时 `bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A)` 画一帧。
   * 可见 → 状态串确实有问题（再逐位查）；
   * 仍不可见 → 状态与可见性无关，直接查属性/矩阵/纹理。
4. 打开 `CNE_METAL_DEBUG=1` 时，把 `[cne-dbg]` 的 `state=0x%llx` 改成"逐位解码"输出（我可以提供 20 行 helper），以后一眼就能看出多余位。

---

## 8. 编译验证

```
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
./metal/scripts/check-compile.sh --lime ~/haxelib-dev/lime-832 \
    --hxcpp ~/haxelib/hxcpp/4,3,2 --out /tmp/obj-agent-states

lime   : /Users/maxwu/haxelib-dev/lime-832
bgfx   : /Users/maxwu/Codename-Metal/bgfx-master
hxcpp  : /Users/maxwu/haxelib/hxcpp/4,3,2

  compiling MetalContext.cpp   ... OK  ( 36K)
  compiling MetalBindings.cpp  ... OK  (448K)

primitive symbols in MetalBindings.o:
  lime_gl_*: 1078
  lime_metal_*: 8

OK - Metal backend compiles against bgfx API v166 and Lime
```

（本次未改任何源码，编译通过；`MetalStates.inc` 仍与 `~/haxelib-dev/lime-832/project/src/graphics/metal/MetalStates.inc` 逐字节一致。）

---

## 9. 信心程度

| 结论 | 信心 |
| --- | --- |
| 两个 state 值的逐位解码（§1） | **高**（可复核，leftover=0） |
| 剔除不是"全不可见"的原因（§2.1） | **高**（采样的 CULL 位为 0，且 OpenFL 主动 `setCulling(NONE)`） |
| 写入掩码恒 RGBA、混合映射正确（§2.3/2.4） | **高** |
| `depthTest` 位来自 OpenFL `configureBackBuffer(..., true, ...)`，不是映射 bug（§2.2） | **高**（调用链逐行确认） |
| "深度缓冲从未被清"这一缺陷存在（§2.2） | **高**（`setViewClear` 替换语义 + OpenFL 只清 COLOR） |
| "深度测试**当前**是否就是黑屏根因" | **中低**：实测位置属性流是 `num=2`（float2）、z 由 Metal 补位为 0（近平面），此时 `LEQUAL` 不会丢片元；只有 z≠0（补位/属性/矩阵异常，或以后引入 GL→Metal 深度换算）才会全丢。补丁 1 无论是否根因都应修 |
| 潜伏缺陷清单（§2.2/2.1 末、§2.3/2.4 末） | **高**（代码即证据） |
