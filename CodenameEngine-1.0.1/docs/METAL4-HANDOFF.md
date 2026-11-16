# Metal 4 原生渲染迁移 — 交接文档

> 给下一个接手这个任务的 AI。  
> 最后更新：2026-11-16  
> 当前分支：`metal4`  
> 主仓库：`/Users/maxwu/Codename-Metal`  
> 引擎目录：`CodenameEngine-1.0.1`  
> 用户仓库：`TheComingUnitys/Codename-Metal`  
> 归属：DeepSeek-V4.1-Flash

---

## 0. 一句话结论

**默认 bgfx 路径正常可玩；原生 Metal 4（MTL4）路径已经打通 queue / command buffer / pipeline / draw / FBO / present，但引擎 draw 仍然全黑，未完成。**

不要声称原生路径完成。不要删除 bgfx。不要默认开启 `CNE_METAL4_NATIVE`。

---

## 1. 用户明确要求与约束

- 把 Codename Engine 1.0.1 的渲染后端移植到 macOS Metal。
- 用户已授权：
  - 用 **Apple Metal 4** 替换 bgfx；
  - 使用 `apple/metal-cpp`（commit `27c4382b7151d55a51692cdcb27aaa98752240de`）；
  - 删除 bgfx 依赖（**但只能在原生路径完全等价后**）。
- 当前阶段默认路径必须保持可构建、可玩。
- 只在用户明确要求时构建 universal / release。
- **不要替用户启动游戏**。用户自己运行测试并把截图/日志发回来。
- 不要不到一分钟就汇报；尽量多做再汇报（用户多次强调）。
- 归属写 **DeepSeek-V4.1-Flash**。

---

## 2. 仓库与分支状态

```text
cd /Users/maxwu/Codename-Metal
git branch --show-current        # metal4
git log --oneline -3
605f749 metal4: VAO-size descriptors, alpha constants and diagnostics
04e3073 metal4: MSL attribute reflection and debug triangle
2f1bd96 metal4: experimental native draw path (env gated)
```

交接时工作区还有未提交的调试改动：

```text
M CodenameEngine-1.0.1/metal/native/Metal4App.mm
M CodenameEngine-1.0.1/metal/native/MetalNative.inc
```

这些是 `CNE_METAL4_DEBUG_ENGINE` 的纯洋红调试 shader、索引日志等。见第 10 节。

OpenFL fork 也有未提交改动（必须保留/理解）：

```text
cd /Users/maxwu/Codename-Metal/forks/openfl-metal
git status --short
 M src/openfl/display/OpenGLRenderer.hx
 M src/openfl/display/Shader.hx
 M src/openfl/display/_internal/ShaderBuffer.hx
 M src/openfl/display3D/Context3D.hx
```

它们分别做：
- `ShaderBuffer.current` 静态指针；
- `Shader.__update()` 在 native draw 前用当前 ShaderBuffer 刷 uniform；
- `Context3D.drawTriangles` 在 `MetalNative.drawIndexed` 前调用 `__state.shader.__update()`；
- renderer 设置/清空 `ShaderBuffer.current`。

**注意：这些 Haxe 改动是实验性的，默认路径下也可能被调用，必须做回归。**

---

## 3. 构建方法（用户机器）

不要用 Xcode 的 `metal` 工具；本机只有 CLT，AOT metallib 通过 `CompileShaderAot.mm` 生成。

标准 arm64 debug/app 构建：

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
export MACOSX_DEPLOYMENT_TARGET=11.0

./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --skip-aot --skip-metallib

cd ~/haxelib-dev/lime-832
haxelib run lime rebuild mac -Dmetal -Dmetal_instancing -DMACOSX_DEPLOYMENT_TARGET=11.0
echo "ndll=$?"

cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --skip-aot --skip-metallib --deploy
```

如果改了 Haxe/OpenFL 源码，还要构建 app：

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
haxelib run lime build mac -Dmetal -Dmetal_instancing -DMACOSX_DEPLOYMENT_TARGET=11.0
```

打包到用户测试路径：

```sh
rm -rf /Users/maxwu/Downloads/CodenameEngine-arm64-m4native.app
cp -R export/release/macos/bin/CodenameEngine.app /Users/maxwu/Downloads/CodenameEngine-arm64-m4native.app
```

运行（由用户执行，不要代跑）：

```sh
CNE_METAL4_NATIVE=1 /Users/maxwu/Downloads/CodenameEngine-arm64-m4native.app/Contents/MacOS/CodenameEngine > /tmp/cne-native.log 2>&1
```

默认（bgfx）路径：

```sh
/Users/maxwu/Downloads/CodenameEngine-arm64-m4native.app/Contents/MacOS/CodenameEngine
```

### 构建陷阱

- `lime rebuild mac` 可能很慢（10–30 分钟），不要因为没输出就杀。
- 判断成功要看 `ndll=0` / `app=0`，不要 `grep -c 'error:'`：Apple 警告文本里经常包含 `error:` 字样，会误报。
- `Metal4App.mm` 在设备/模拟器两份源码里各有一份（lime 的 `src/graphics/metal` 与原生目录），`apply.sh` 会镜像。改原生文件后重新跑 `apply.sh`。
- 不要用 `read` 默认 2000 行后整文件 `write` 回写大文件（曾把 `Context3D.hx` 截断）。用 `edit` 做局部替换，或先备份/恢复。
- Git push 有时因认证卡住；本地提交优先，push 可稍后重试。

---

## 4. 当前状态矩阵

| 项目 | 状态 |
|---|---|
| 默认 bgfx 路径 | **正常可玩** |
| MTL4 queue/allocator/command buffer | 正常 |
| MTL4 render pass / target 切换 | 正常 |
| MTL4 pipeline（AOT metallib） | 正常 |
| MTL4 纹理上传/采样 | 正常（独立测试） |
| MTL4 present | 正常 |
| 独立调试三角形 | **可见** |
| FBO 创建 + clear + 采样 + composite | **可见**（强制洋红能看到） |
| 引擎 draw（正常 shader） | **全黑** |
| 引擎 draw（纯洋红、无纹理/无 uniform 调试 shader） | **全黑** |
| Metal validation / abort | 当前版本无 |

最后一项是核心未解问题：连“纯洋红、只读 VAO position/uv”的调试 shader 都不出像素，说明问题不在引擎 shader/uniform/纹理，而在 **native draw 消费引擎 VAO/索引数据的桥接层**。

---

## 5. 架构与文件地图

### 原生后端

`CodenameEngine-1.0.1/metal/native/Metal4App.h`
- C ABI：init / frame_begin / target / viewport / scissor / texture / buffer / program / draw / shutdown。

`CodenameEngine-1.0.1/metal/native/Metal4App.mm`
- MTL4 实现。
- `gDevice` / `gQueue` / `gAllocator` / `gCommandBuffer`。
- screen target = `gTarget == -1`，FBO target = texture id。
- 每个 target 单独 pipeline（BGRA8 screen / RGBA8 FBO）。
- 两套 argument table：
  - VS：uniform buffer(0) + vertex buffers(1..N)
  - FS：uniform buffer(0) + textures + sampler
- 临时 uniform/index/const buffer 存 `gTransient`，下一帧释放。
- `GetPipeline` 构建 vertex descriptor / pipeline。
- 当前加了调试代码：
  - `DrawDebugTriangle()`（`CNE_METAL4_DEBUG_DRAW`）
  - `DrawDebugFbo()`（`CNE_METAL4_DEBUG_FBO`）
  - `CNE_METAL4_DEBUG_ENGINE` 替换引擎 shader 为纯洋红调试 shader

`CodenameEngine-1.0.1/metal/native/MetalNative.inc`
- `Metal4Draw()`：从 Haxe/ProgramData/VAO/ShaderData 收集数据并调用 `cne_metal4_app_draw`。
- `cne_metal_draw_indexed` / `cne_metal_draw_arrays` 在 MTL4 active 时直接进原生路径。
- MSL 属性反射、generic constant、viewport/scissor 转发。
- 当前有大量诊断日志（`mslattr` / `idx` / `vd` / `tex` / `matrixRaw`）。

`CodenameEngine-1.0.1/metal/native/MetalContext.cpp`
- `CNE_METAL4_NATIVE=1` 初始化、frame begin/end、target 切换。
- `CNE_METAL4_CLEAR=1` 旧的 capture-present 调试路径（慢，不要当完成）。
- `CNE_METAL4_DEBUG_CLEAR=1` 强制洋红清屏。
- `CNE_METAL4_DEBUG_FBO_CLEAR=1` 强制 FBO 清屏洋红。

`CodenameEngine-1.0.1/metal/native/MetalBindings.cpp`
- `lime_gl_bind_framebuffer` 钩子：切 screen / FBO target、尺寸、清屏。

`CodenameEngine-1.0.1/metal/native/MetalTextures.inc`
- 纹理缓存、`genericAttribs`（OpenFL 的 constant vertex attributes）、`attribDivisors`。
- `SetGenericAttribF/I` 在这里。

### Haxe / OpenFL

`forks/openfl-metal`（通过 `apply.sh` 同步到 `~/haxelib-dev/lime-832` 和引擎源码）。
关键改动见第 2 节。

`metal/haxe/lime/graphics/metal/MetalNative.hx`
- 原生绑定 + stage 开关（`state` / `geo` / `upload` / `prog`）。

`metal/haxe/lime/graphics/metal/ShaderAotCache.hx`
- AOT metric 缓存，59 个 metallib，入口 `xlatMtlMain`。

### 文档

`CodenameEngine-1.0.1/docs/METAL4-NATIVE-RENDERER.md`
- 迁移状态记录（之前约 93–95%）。

`CodenameEngine-1.0.1/docs/wiki/` + GitHub Wiki
- Metal 后端文档、依赖快照。

---

## 6. 运行时环境变量

| 变量 | 作用 |
|---|---|
| `CNE_METAL4_NATIVE=1` | 启用原生 MTL4 路径（实验，仍黑屏） |
| `CNE_METAL4_CLEAR=1` | 旧 capture-present 调试路径（慢） |
| `CNE_METAL4_DEBUG_CLEAR=1` | 强制整屏洋红清屏 |
| `CNE_METAL4_DEBUG_DRAW=1` | 画独立调试三角形（不依赖引擎数据） |
| `CNE_METAL4_DEBUG_FBO=1` | 用全屏三角形采样 texture 0（不可靠的 FBO 查看器） |
| `CNE_METAL4_DEBUG_FBO_CLEAR=1` | 强制 FBO 清屏洋红 |
| `CNE_METAL4_DEBUG_ENGINE=1` | 引擎 VAO/索引 + 纯洋红调试 shader（当前关键诊断） |
| `CNE_METAL4_NO_SCISSOR=1` | 跳过 native scissor（已排除 scissor 问题） |
| `CNE_METAL_NATIVE_PATH=state,geo,upload` | 让 prog 走 GL 路径的对照测试（已试，仍黑） |

---

## 7. 已修复/已确认的关键技术点

### 7.1 MSL 属性槽位

AOT metallib 里的属性按 **bgfx Attrib 枚举**编号，不是 GL location，也不是声明顺序：

```text
a_position    [[attribute(0)]]
a_normal      [[attribute(1)]]
a_tangent     [[attribute(2)]]
a_bitangent   [[attribute(3)]]
a_color0      [[attribute(4)]]   // OpenFL TextureCoord
a_color1      [[attribute(5)]]   // OpenFL Alpha (float)
a_color2      [[attribute(6)]]   // ColorMultiplier (float4)
a_color3      [[attribute(7)]]   // ColorOffset (float4)
```

OpenFL 名称映射：

```text
openfl_Position       -> a_position  (slot 0)
openfl_TextureCoord   -> a_color0    (slot 4)
openfl_Alpha          -> a_color1    (slot 5)
openfl_ColorMultiplier-> a_color2    (slot 6)
openfl_ColorOffset    -> a_color3    (slot 7)
```

之前按 GL location 或声明顺序编号，导致 `a_position(0) is missing` / tangent 缺失。

### 7.2 Vertex descriptor 规则（Metal 很严格）

- `stepFunction = Constant` 时 **`stepRate` 必须为 0**，否则：
  `stepRate(1) must be zero if stepFunction is MTLVertexStepFunctionConstant`
- **没有 attribute 引用的 buffer layout 不能设置 stride**，否则：
  `None of the attributes set bufferIndex to 2, but ... layout[2].stride(4)`
- 同一个 MSL slot 不能被 VAO 和 generic constant 同时写；后写会覆盖前写并让真实 layout 变空。
- descriptor 的 attribute format 必须和 MSL 类型一致（Float / Float2 / Float3 / Float4）。

### 7.3 实际 VAO 布局

日志实测：

```text
prog=10: stride 56, position vec3 offset 0, texcoord vec2 offset 12
prog=16: stride 16, position vec2 offset 0, texcoord vec2 offset 8
```

**不能统一按 vec3/vec4 建 descriptor。** 必须按 VAO 实际 size / stride。

### 7.4 两层 uniform

VS 和 FS 的 MSL uniform struct 都在 `buffer(0)`，但布局不同：

```text
prog=16 VS: openfl_Matrix(0), openfl_HasColorTransform(64),
            openfl_TextureSize(80), hasColorTransform(96)
prog=16 FS: hasTransform(32), _camSize(64)
```

必须拆成 `vsBytes` / `fsBytes`，分别绑定 VS/FS argument table。合并会重叠并破坏矩阵。

### 7.5 OpenFL 矩阵/ShaderBuffer 问题

- `openfl_Matrix` 在引擎 draw 的 uniform 缓存里经常只有平移，缩放列为 0（甚至全 0）。
- 这是 ShaderBuffer override 没有可靠进入 GLSL 参数导致的。
- 已加兜底：矩阵退化时用当前 target 尺寸的正交投影替换。
- 已加 Haxe 侧 `ShaderBuffer.current` + `Shader.__update()`，但仍不完整。
- **不要假设矩阵正确**；每帧打印 `matrixRaw` 验证。

### 7.6 索引偏移

`cne_metal_draw_indexed` 的 `byteOffset` 之前被忽略，所有 draw 都从索引 0 读。已改为索引地址 + offset、长度减去 offset。

### 7.7 临时 buffer 生命周期

uniform/index/constant buffer 若只用局部 `id`，ARC 可能在 GPU 执行前释放。必须 retain 到下一帧（`gTransient`）。

### 7.8 纹理

纹理有 CPU 像素（日志示例）：

```text
tex id=17 1248x260 pixels=1297920
tex id=19 1280x124 pixels=634880
```

尺寸/上传正常；不要先怀疑纹理为空。

---

## 8. 当前黑屏的证据链

关键日志/实验：

1. `CNE_METAL4_DEBUG_DRAW=1`：独立三角形**可见** → MTL4 draw/present 正常。
2. `CNE_METAL4_DEBUG_FBO_CLEAR=1` + `CNE_METAL4_DEBUG_FBO=1`：窗口**洋红** → FBO clear + composite 正常。
3. 正常引擎 draw：全黑。
4. `CNE_METAL4_DEBUG_ENGINE=1`（纯洋红片元、无纹理/无 uniform、float2 position+uv）：**仍全黑**。
5. 索引/顶点日志：

```text
idx prog=16 count=6 first=0,1,2,2,1,3 stride0=16 vb0size=128
  v0 = 16.00 314.00 0.00
  v1 = 1264.00 314.00 1.00
  v2 = 16.00 574.00 0.00
```

6. 没有 Metal validation 错误，没有 pipeline 失败。
7. `CNE_METAL4_NO_SCISSOR=1` 仍黑 → 不是 scissor。
8. `CNE_METAL_NATIVE_PATH=state,geo,upload` 仍黑 → 不是单纯 prog stage 问题。

**这意味着：native draw 调用发生了，但实际 GPU 读到的 vertex/index/argument table 与 C++ 侧日志不一致。**  
可能原因（按优先级）：
- argument table 的 buffer 绑定 index 与 descriptor bufferIndex 不一致（目前代码看起来一致，但要用 capture 验证）。
- 引擎 draw 使用的 buffer 在 draw 前被后续 upload 覆盖（CPU 镜像 vs GPU buffer 时序）。
- draw 的 `baseVertex` / `indexType` / `primitive` 参数与日志假设不一致。
- `MTL4ArgumentTable` 在多次 draw 之间复用/未重置，导致后续 draw 绑定错。
- VAO 的 buffer 在 native 路径下不是我们以为的 id（`vao.attribs` 与当前实际绑定不一致）。

---

## 9. 下一个 AI 应该先做什么

### 第一优先：抓 GPU capture，不要继续盲改

在 `Metal4App.mm` 的 `cne_metal4_app_frame_begin` 首次调用时启动：

```objc
MTLCaptureManager* mgr = [MTLCaptureManager sharedCaptureManager];
MTLCaptureDescriptor* desc = [MTLCaptureDescriptor new];
desc.captureObject = gQueue; // 或 gDevice
desc.destination = [NSURL fileURLWithPath:@"/tmp/cne-frame.gputrace"];
NSError* err = nil;
[mgr startCaptureWithDescriptor:desc error:&err];
```

第 2 帧 `stopCapture`。然后让用户用 Xcode 打开 `/tmp/cne-frame.gputrace`，看：

- 第一个引擎 draw 的 vertex buffer 实际内容
- index buffer
- VS 输入/输出
- argument table 绑定
- 纹理采样

**不要在没有 capture 的情况下继续改 descriptor。**

### 第二优先：验证 argument table

在 `Metal4App.mm` 的 draw 里，对每个 vertex buffer：
- 打印 `bufferIds[i]`、`gpuAddress`、`strides[i]`、descriptor 的 `bufferIndex`；
- 确认 `setAddress:attributeStride:atIndex:` 的 index 与 `attribBuf[i] + 1` 完全一致；
- 确认每次 draw 前 argument table 是新建/重置的，不复用上一次的绑定。

### 第三优先：用 CPU 读回 FBO

在 FBO target 结束后 `getBytes` 读回 4x4 像素，打印非零数量。  
如果 FBO 全 0 → draw 没写入；如果 FBO 有颜色但屏幕黑 → composite 问题（目前看 composite 是好的）。

### 第四优先：简化引擎 draw

用固定的 3 个顶点（不读引擎 buffer）画到 FBO，确认 FBO target 的 draw 正常；再逐步换成引擎 buffer。  
注意：调试 shader 的 attribute 类型必须与实际 descriptor 一致，否则会静默失败。

---

## 10. 当前未提交的调试代码

`Metal4App.mm`：
- `DrawDebugTriangle` / `DrawDebugFbo`
- `gDebugEngine` / `gDebugEngineLib`
- GetPipeline 里 `gDebugEngine` 时用内联 MSL：
  ```metal
  struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(4)]]; };
  vertex ... o.pos = float4(in.pos, 0.0, 1.0);
  fragment ... return float4(1.0,0.0,1.0,1.0);
  ```
- `CNE_METAL4_DEBUG_ENGINE` 下强制 position comps=2。

`MetalNative.inc`：
- 索引日志（`idx`、`v0/v1/v2`）
- 强制 alpha/颜色常量
- MSL 反射缓存 `gMslAttrCache`
- 大量 `fprintf` 诊断

这些都应在新会话里决定：保留为诊断开关，还是清理。

---

## 11. 已知陷阱/不要重复踩

1. **不要用默认 2000 行 read 后整文件 write**。会截断大文件（`Context3D.hx` 2934 行被截成 1140）。用 `edit` 局部改。
2. **不要相信 `grep -c 'error:'` 判断编译失败**；用 `ndll=$?` / `app=$?`。
3. **不要在没有 `ndll=0` 的情况下 deploy**。
4. **不要默认启用 `CNE_METAL4_NATIVE`**。
5. **不要删除 bgfx**，直到原生路径完整通过回归。
6. **不要替用户运行游戏**。
7. **不要声称原生路径完成**；它现在仍黑屏。
8. **不要只改 `forks/openfl-metal` 就以为生效**；`apply.sh` 会同步，需重新构建 app。
9. `CNE_METAL4_DEBUG_FBO` 采样的是 `gTextures[0]`，很多时候 texture 0 不存在，所以它不可靠；洋红画面可能来自引擎自己的 composite，不是 debugFbo。
10. Metal 4 需要 macOS 26+ / `MTLGPUFamilyMetal4`；本机 M3 + macOS 26.6.2 支持。macOS 11 deployment target 只是编译目标，运行时必须 26+。
11. 设备/模拟器两份 `Metal4App.mm` 要同步（`apply.sh` 会镜像）。
12. AOT shader cache 入口固定 `xlatMtlMain`；不要改成别的名字。
13. MSL 属性 slot 是 bgfx enum，不是 GL location；这是最容易错的地方。
14. shader 的 `[[attribute(n)]]` 和 descriptor 的 `bufferIndex` 是两个不同空间：前者是 MSL slot，后者是 vertex buffer 槽。

---

## 12. 还需要做的回归（原生路径修好后）

- 主菜单 / 歌曲 / 暂停 / 退出
- FBO / render-to-texture / filters
- 窗口 resize / Retina scale
- blend / depth / scissor / stencil
- 纹理 UV 翻转
- 绘制顺序 / 遮罩
- 至少 3 首歌 + 菜单循环
- 性能目标 60 FPS
- 内存（transient buffer 是否持续增长）

只有全部通过，才考虑：
- 删除 capture 路径
- 删除 bgfx 构建依赖
- universal / release 构建

---

## 13. 有用的日志命令

```sh
# 是否还有 Metal validation
grep -E 'failed assertion|pipeline failed|abort' /tmp/cne-native.log | tail

# 引擎 draw 次数
grep -c '\[cne-metal4\] draw' /tmp/cne-native.log

# MSL 属性
grep '\[cne-metal4\] mslattr' /tmp/cne-native.log | head -20

# descriptor
grep '\[cne-metal4\] vd' /tmp/cne-native.log | head -6

# 索引/顶点
grep -A 4 '\[cne-metal4\] idx' /tmp/cne-native.log | head -20

# 矩阵
grep '\[cne-metal4\] matrixRaw' /tmp/cne-native.log | head -8

# 纹理
grep '\[cne-metal4\] tex' /tmp/cne-native.log | head -10
```

---

## 14. 交接检查清单

- [ ] 读懂第 8 节证据链，不要重复“猜 descriptor”
- [ ] 确认工作区两个修改文件的用途
- [ ] 确认 OpenFL fork 四个 Haxe 改动是否要保留
- [ ] 先做 GPU capture / CPU readback
- [ ] 每次构建确认 `ndll=0` / `app=0`
- [ ] 不默认开 native、不删 bgfx、不代跑游戏
- [ ] 修好后先跑第 12 节回归，再谈删 bgfx

---

## 15. 参考提交

```text
605f749 metal4: VAO-size descriptors, alpha constants and diagnostics
04e3073 metal4: MSL attribute reflection and debug triangle
2f1bd96 metal4: experimental native draw path (env gated)
aaa7b22 metal4: fix native draw attribute slots, sampler, format and per-stage uniforms
e0d431d (更早的 MTL4 基础提交)
```

默认 bgfx release 在 main 分支：v1.0.1-metal.5（历史信息，未在当前分支验证）。
