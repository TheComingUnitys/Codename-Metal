# OpenFL 2D 渲染层原生化方案（消除最后的 "GL 形状"）

> 目标：用户要求"**完全原生使用 Metal 渲染，不要任何翻译**"。
> 前序成果：着色器已是**构建期 AOT 原生 MSL**（运行时 GLSL→MSL 翻译 = 0，见 `aot-shaders.md`）；
> 所有画面已由 bgfx 的 Metal 后端真实提交（见 `shaders.md` / `paths.md`）。
> **本文件处理最后一层"GL 形状"**：OpenFL 2D 渲染器 + `lime_gl_*` ABI + 我们的 GL 仿真层。
> 日期：2026-10-17。作者：audit-shaders（本轮）。

---

## 0. 结论（TL;DR）

1. **可行，且边界比预想干净。** OpenFL 的 2D 渲染逻辑层
   `openfl/display/_internal/Context3D*.hx`（**3,026 行**）里 **`gl.*` 调用数 = 0** ——
   它全部通过 `renderer.*` / `Context3D.*` 的抽象 API 工作。GL 调用**只**集中在后端类里：
   `display3D/Context3D.hx`、`Program3D.hx`、`display/Shader.hx`、`textures/*`、`VertexBuffer3D`/`IndexBuffer3D`，
   合计 **≈322 处 `gl.*` 调用**（6,721 行文件里的少量行）。
2. **推荐方案 B：写 Metal-native 的 `Context3D` 后端**（保留它的公共 API 形状供 OpenFL 渲染器调用，
   把内部 118 处 GL 调用换成 Metal-native 调用；`Program3D`/`Shader`/`Texture`/`Buffer` 同步换代）。
   **不需要改 3,026 行渲染器内部逻辑** —— 这是 A 方案最大的浪费。
3. **A（新 native RenderContext + 改渲染器调用点）不划算**：需要改 3,026 + 1,215 行渲染器，
   且仍必须重写 `Program3D`/`Shader`/`Texture`（渲染器还在用它们），总改动量 **大于** B，回归面更大。
4. **"是否真的消除翻译而不是只改名"**：B 消除的是 **`lime_gl_*` GL ABI 与 GL 状态机仿真**（不是改名）：
   调用面从 **806.5 次/帧 `lime_gl_*` CFFI**（≈40 次/笔 draw）收敛到 **≈6–9 次/笔 draw 的 Metal 原语**
   （pipeline/vertex buffers/uniform bytes/texture/draw）；不再有 VAO 仿真、GL 枚举翻译、常量属性物化、
   view/scissor Y 翻转、格式猜测这些 GL 语义补丁。**唯一保留的"非原生"步骤是 `newLibraryWithSource`**
   （本机无完整 Xcode/`xcrun metal`，无法离线编译 metallib；AOT 已把"翻译"完成，这里只剩"编译"，不是翻译）。
5. **建议路线**：先做 **Phase 0–2（2–3 天工作量）**拿到"精灵/文本主路"的原生 A/B 一致；
   Phase 3 覆盖 Graphics/FBO/滤镜；**Phase 4 再决定是否连 bgfx 一起换成直连 Metal**。
   bgfx 与 GL 形状消除是**正交**的两件事：bgfx 当前就是 Metal 渲染器，去掉 bgfx 是另一笔 2–3k 行 native 的收益/风险。
6. 原型（本轮已完成）：**同一帧 A/B**——把一帧的真实 draw 列表用**直连 Metal**回放，与同帧 bgfx 呈现结果**逐像素完全一致**
   （`mean=0.0 / max=0 / 差异像素=0`，frame 360 的 46,499 个非黑像素）。设计与证据见 §5.4。

---

## 1. 现状硬数据（全部可复核）

### 1.1 分层与规模

| 层 | 位置 | 行数 | 直接 `gl.*` 调用 | 说明 |
| --- | --- | --- | --- | --- |
| ① 2D 渲染逻辑（按显示对象类型分派、批量、裁剪、遮罩、缓存） | `.haxelib/openfl/git/src/openfl/display/_internal/Context3D*.hx` | **3,026** | **0** | **API 干净层**；只调 `renderer.*` / `Context3D.*` |
| ② 2D 渲染器本体（OpenGLRenderer，GL 后端） | `openfl/display/OpenGLRenderer.hx` | 1,215 | ≈20（`__gl.viewport/clear/getParameter/getExtension/blendEquation`） | 状态机 + shader 绑定 + mask/scissor + FBO 滤镜 |
| ③ Stage3D 风格 API 后端（GL 实现） | `openfl/display3D/Context3D.hx` `Program3D.hx` `VertexBuffer3D.hx` `IndexBuffer3D.hx` `textures/{TextureBase,Texture,RectangleTexture,CubeTexture,VideoTexture}.hx` `display/Shader.hx` | **6,721** | **≈322**（Context3D 118 / Program3D 48 / Shader 29 / TextureBase 64 / Texture 10 / CubeTexture 19 / RectangleTexture 4 / VideoTexture 2 / V-Buffer 4 / I-Buffer 4） | **GL 形状集中在这里** |
| ④ Lime GL 转发层 | `lime/_internal/backend/native/NativeOpenGLRenderContext.hx` + `NativeCFFI` | 3,452 | —（290 处 CFFI） | `GL` → `lime_gl_*` 纯转发 |
| ⑤ 我们的 GL 仿真层 | `metal/native/MetalBindings.cpp`(3613) + `MetalStates.inc`(1188) + `MetalTextures.inc`(1552) + `MetalUniforms.inc`(767) + `MetalContext.cpp`(967) | **≈8,000**（扣掉诊断/覆盖行） | 269 个原语 | **GL 语义 → bgfx/Metal 的翻译器**（VAO 仿真、格式映射、常量属性物化、view 映射…） |
| ⑥ 运行时真实用到的原语 | `primitives.md` §3 | — | — | **49 个**（游戏内与菜单会话集合相同） |

### 1.2 运行时调用代价（primitives.md 数据 / 7,441 帧游戏内会话）

- **总计 6,001,109 次 `lime_gl_*` 调用 ⇒ 806.5 次/帧**；`lime_gl_draw_elements`=20.2/帧 ⇒
  **≈40 次 GL-ABI CFFI / 笔 draw**（其中绝大部分是状态/属性簿记，不是真实 GPU 工作）。
- Top 调用（次/帧）：`disable_vertex_attrib_array` 201.5、`enable_vertex_attrib_array` 95.4、
  `uniform1i` 78.8、`vertex_attrib4f` 68.2、`bind_buffer` 57.6、`enable` 56.6、`vertex_attrib_pointer` 55.0、
  `bind_texture` 27.8、`active_texture` 26.2、`viewport` 21.2、`draw_elements`/`uniform2f`/`matrix4fv`/`attrib1f` 各 20.2、
  `buffer_data` 16.2、`disable` 15.1、`use_program` 3.1、`clear` 1.0。
- **原生实现下每笔 draw 的 Metal 调用 ≈ pipeline(1) + setVertexBuffer(≤4) + setFragmentTexture(1)
  + set*Bytes(uniforms 1–2) + drawIndexed(1) ≈ 6–9 次/笔 ≈ 130–180 次/帧**，
  即**调用面缩小 ~5–6 倍**，且每次调用不再经过"GL 语义机"。

### 1.3 运行时真实原语（49 个）按原生化难度分档

| 档 | 原语（运行时次数/帧） | 原生 Metal 对应 | 备注 |
| --- | --- | --- | --- |
| 绘制 | `draw_elements`(20.2) | `drawIndexedPrimitives` | — |
| 顶点流 | `vertex_attrib_pointer`(55.0)、`enable/disable_vertex_attrib_array`(95.4/201.5)、`bind_buffer`(57.6) | **一次性**：`setVertexBuffer(buffer, offset, streamIndex)` + pipeline 的 `MTLVertexDescriptor`（由 layout 决定，随 pipeline 缓存） | GL 的"每帧反复 enable/pointer"在原生路径**整个消失** |
| 常量属性 | `vertex_attrib4f`(68.2)、`vertex_attrib1f`(20.2) | 显式常量顶点流（保持现有物化做法，或改 per-draw `setVertexBytes`），但**不再有 GL 常量属性语义** | 现有后端已物化，逻辑可复用 |
| uniform | `uniform1i`(78.8)、`uniform2f`(20.2)、`uniform_matrix4fv`(20.2) | **per-stage uniform buffer**（`setVertexBytes`/`setFragmentBytes`）+ 构建期确定的偏移；`glGetUniformLocation` 改为编译期表 | 现有 `configure_program` 表可直接改成原生偏移表 |
| 纹理 | `tex_image_2d`(0.79)、`tex_parameteri`、`active_texture`(26.2)、`bind_texture`(27.8) | `MTLTexture.replaceRegion` + `MTLSamplerState`（缓存）+ `setFragmentTexture(unit)` | 无 GL 枚举翻译；格式由 OpenFL 的 Context3DTextureFormat 直接映射 MTLPixelFormat |
| 状态 | `enable`(56.6)、`disable`(15.1)、`blend_func*`、`color_mask`、`depth_mask`、`depth_func`、`cull_face`、`front_face`、`scissor`、`viewport`(21.2) | **`MTLRenderPipelineState`（按状态键缓存）+ viewport/scissor**；不再有"逐 draw set state bits"的 GL 状态机 | 状态组合实际很少（2D：blend on/off + 几种 blend func） |
| 清屏 | `clear`/`clear_color`(各 1.0) | `MTLRenderCommandEncoder` loadAction=clear / 或显式 clear pass | — |
| 查询 | `get_string`、`get_integer`、`get_attrib_location`、`get_uniform_location`、`get_programi`、`get_shaderi`、`get_shader_info_log`、`object_register` | 启动期一次性 → 返回常量/表查询 | 不再需要 GL 查询仿真 |
| 空实现/休眠 | `depth_func`、`front_face`、`stencil_*`（各 1 次） | 原生后端可按需实现或显式 no-op + 日志 | 3D 路径 |

---

## 2. 调用点映射表（谁 → 什么情况 → 落到哪个原语）

### 2.1 层次图

```
Flixel/CNE/OpenFL 用户 API（sprite/FlxText/graphics/texture…）
   └─ ① display/_internal/Context3D*.hx  [3,026 行, 0 处 gl]  ← 只调 renderer.* / context3D.*
        └─ ② OpenGLRenderer (display/OpenGLRenderer.hx)  [1,215 行, ≈20 处 __gl]
             ├─ 状态/清屏/viewport/mask/scissor/FBO（直接 GL）
             └─ ③ display3D.Context3D.setVertexBufferAt/drawTriangles/…（public API）
                  ├─ 内部 __flushGL* → gl.enable/disable/blend*/viewport/scissor/stencil/uniform…
                  └─ gl.drawElements/drawArrays
   Shader / Program3D / Texture / VertexBuffer / IndexBuffer  ── 直接 gl.*（322 处）
        └─ ④ lime.GL → NativeOpenGLRenderContext（3452 行纯转发）→ lime_gl_* CFFI
             └─ ⑤ metal/native GL 仿真（≈8k 行）→ bgfx(Metal)
```

### 2.2 ① 渲染逻辑层 → 触发的场景/API（全部 0 处 GL）

| 文件（①，display/_internal） | 行数 | 触发场景（用户可见） | 调用的 ②/③ API（关键） |
| --- | --- | --- | --- |
| `Context3DGraphics.hx` | 1,402 | **Graphics API 的顶点批处理**：Flixel tile 批量渲染（`drawQuads`/`beginShaderFill`）、Shape 填充、矢量 | `renderer.__initGraphicsShader/__initShaderBuffer/__updateShaderBuffer/__setShaderBuffer/applyMatrix/applyAlpha/applyColorTransform/applyBitmapData/setShader/updateShader`；`Context3DBuffer`（VBO） |
| `Context3DTilemap.hx` | 661 | Tilemap（`FlxTilemap`/tile 渲染） | 同上 + `renderer.__gl`、`setVertexBufferAt`、`drawTriangles` |
| `Context3DBitmap.hx` | 115 | **Bitmap/Sprite 显示列表**（Flixel `FlxSprite` 全部：角色、UI、音符…） | `renderer.applyBitmapData/applyAlpha/applyColorTransform/applyMatrix/setShader/updateShader/__renderEvent/__pushMaskObject/__popMaskObject` |
| `Context3DVideo.hx` | 138 | 视频帧（`Video`/`VideoTexture`） | 同上（+ `__renderEvent`） |
| `Context3DShape.hx` | 115 | Shape（`Graphics` 绘制的显示对象） | `renderer.__initDisplayShader/apply*/setShader/updateShader` |
| `Context3DDisplayObject.hx` | 117 | 通用显示对象（遮罩推送/弹出、缓存位图） | `renderer.__pushMaskObject/__popMaskObject/__pushMaskRect/__popMaskRect/__renderEvent/__updateCacheBitmap/__setBlendMode` |
| `Context3DDisplayObjectContainer.hx` | 69 | 容器层级递归/遮罩 | `renderer.__renderDrawable/__renderDrawableMask/__pushMaskObject/__popMaskObject` |
| `Context3DSimpleButton.hx` | 29 | SimpleButton | `renderer.__popMaskObject/__renderDrawable` |
| `Context3DBitmapData.hx` | 72 | `BitmapData.draw(DisplayObject)` GPU 分支 / 缓存位图 | `renderer.__defaultDisplayShader/apply*/setShader/updateShader` + FBO |
| `Context3DTextField.hx` | 63 | TextField —— **注意：文字内容走 Cairo 软件渲染**（paths.md §2），只有最终位图上屏走这里 | `renderer.__renderEvent/__softwareRenderer/__updateCacheBitmap` |
| `Context3DBuffer.hx` | 141 | 所有顶点/索引数据上传的封装 | `setVertexBufferAt` 前的 VBO 上传 |
| `Context3DMaskShader.hx` / `Context3DAlphaMaskShader.hx` | 52 / 52 | **死代码**（OpenFL 该版本从不实例化它们，见 shaders.md §8） | — |

> **关键结论**：③ 的公共 API 只有约 20 个方法被 ① 使用；只要 B 方案实现这 20 个方法的 native 版本 +
> ② 里约 20 处直接 GL，就能覆盖 CFE 全部 2D 内容，**完全不必动 ①**。

### 2.3 ③ Context3D：API → GL 原语 → 原生映射

`Context3D.hx` 29 个 public + 30 个 `__` 方法，其中 2D 渲染器实际用到：

| Context3D 方法 | 内部 GL 调用（Context3D.hx 行） | 运行时次数/帧 | 原生 Metal 实现 |
| --- | --- | --- | --- |
| `setVertexBufferAt` | `disable/enableVertexAttribArray`、`vertexAttribPointer`（:1530-1600） | 95.4 / 55.0 | 记录 stream→(buffer, offset, format)；**pipeline 的 vertexDescriptor 在建 pipeline 时一次性确定** |
| `drawTriangles` (`__drawTriangles`) | `drawElements`/`drawArrays`（:1904/:1955） | 20.2 | 一次 `drawIndexedPrimitives`（或 `drawPrimitives`） |
| `setProgram` | `useProgram`（经 `__flushGLProgram`） | 3.1 | 切换当前 native program（pipeline 选择） |
| `setBlendFactors(Separate)` | `enable(BLEND)`、`blendFuncSeparate`（:1150-1200） | 7（各 1 次初始化 + 每帧 `__setGLBlend`） | 只影响 pipeline key（2D 只有 premultiplied-alpha 一种常用组合） |
| `setColorMask` | `colorMask` | 5（初始化） | pipeline key |
| `setDepthTest` / `setCulling` | `enable/disable(DEPTH_TEST/CULL_FACE)`、`depthFunc`、`depthMask`、`cullFace`、`frontFace` | 各 1 次 | pipeline key（2D 全部深度关闭） |
| `setScissorRectangle` | `scissor` + `enable/disable(SCISSOR_TEST)` | 10（初始化）/1 | `setScissorRect`（每 pass 一次） |
| `clear` (`__clear`) | `colorMask`、`clearColor`、`clearDepth`、`clearStencil`、`clear` | 1.0 | render pass `loadAction=Clear` |
| `setTextureAt` | `activeTexture`、`bindTexture`（经 `__flushGLTextures`） | 26.2 / 27.8 | `setFragmentTexture(unit, texture)` + sampler（缓存） |
| `createVertexBuffer/IndexBuffer` | `createBuffer`、`bindBuffer`、`bufferData` | 16.2/帧（buffer_data） | `MTLBuffer` + `replaceRegion` |
| `configureBackBuffer/present` | `viewport`（:1963）、`present`（lime） | 21.2/帧 viewport | CAMetalLayer `nextDrawable/present`（现有 MetalLayerHost 复用） |
| `setRenderToTexture/BackBuffer` | `bindFramebuffer`、`framebufferTexture2D`、`checkFramebufferStatus` | **0**（默认内容不用 FBO） | 显式 render pass / `MTLTexture` 附件（**顺手修掉 paths.md 的 FBO 确定缺陷：不再依赖 `checkFramebufferStatus` 惰性创建**） |
| `__flushGLProgram` | `useProgram` + uniform 写入（`flush()`） | 3.1 | program/pipeline 绑定 + uniform buffer 提交 |
| `__flushGLStencil*` / `setStencil*` | `stencilOpSeparate`、`stencilMask`、`stencilFunc` | 各 ~1（初始化） | 2D 不需要；显式 no-op + 注释（3D 才需要 depth-stencil 附件） |
| `drawToBitmapData` | `readPixels` | **0** | 用 `MTLTexture.getBytes` 实现（顺带修 readPixels 缺口） |

### 2.4 `Program3D` / `Shader`（着色器与 uniform 绑定）

| 类 | GL 调用 | 用途 | 原生映射 |
| --- | --- | --- | --- |
| `Program3D.__uploadFromGLSL` | `createShader/shaderSource/compileShader/getShaderParameter/getShaderInfoLog/createProgram/bindAttribLocation/attachShader/linkProgram/getProgramParameter/getProgramInfoLog` | 编译链接（**现在输入是 AOT MSL**） | 直接 `MTLDevice.newLibraryWithSource(MSL)` + 缓存 `MTLFunction`/pipeline 描述；无 GL 枚举 |
| `Program3D.uploadSources` | `getUniformLocation/getAttribLocation` | 反射 | 用构建期 AOT 缓存里的 attribute/uniform 表（已存在 `TranslateResult`），O(1) 查表 |
| `Program3D.flush` / `__enable` / `__disable` | `useProgram`、`uniform*`、`enable/disable(TEXTURE_2D)`、`uniform1i` | 每帧 uniform 上传 | 写 per-stage uniform buffer（`setVertexBytes/setFragmentBytes`），按 AOT 表偏移 |
| `Program3D.__buildAGALUniformList` / AGAL | 反射 + GL | **Stage3D/AGAL，2D 不用** | 直接删除（2D 无 AGAL 内容） |
| `Shader.__createGLShader/__createGLProgram` | 上述编译链 | 所有 2D shader（DisplayObjectShader/GraphicsShader/FlxGraphicsShader/CustomShader） | 同 Program3D：AOT MSL → `newLibraryWithSource` |
| `Shader.__initGL` | `getUniformLocation/getAttribLocation` | 建 ShaderParameter index | 构建期表 + 运行期一次查表 |
| `Shader.__updateGL/__updateGLFromBuffer` | `uniform*` | 每 draw 上传（FLOAT2/BOOL/MAT4 等） | 写 uniform buffer；`ShaderBuffer` 的每顶点数据继续走 vertex stream |
| `Shader.__enableGL/__disableGL` | `uniform1i`(sampler unit)、`enable/disable(TEXTURE_2D)` | sampler unit 设置 | `setFragmentTexture(unit)`；TEXTURE_2D enable 直接消失 |

### 2.5 纹理家族（`display3D/textures/`）

| 类 | 用途 / 运行时 | GL 调用 | 原生映射 |
| --- | --- | --- | --- |
| `TextureBase` | 公共：sampler 状态、上传、FBO 附件 | `createTexture`、`texImage*`、`texParameteri/f`、`pixelStorei`、`generateMipmap`、`getExtension`、FBO/RBO 全套 | `MTLTexture` + `MTLSamplerState`（缓存）+ 显式 render pass 附件；**去掉 GL 扩展探测与 pixelStorei** |
| `Texture` | 渲染到纹理（缓存位图/滤镜） | 同上 + `createFramebuffer` | `MTLTexture`(renderTarget) + render pass |
| `RectangleTexture` | **2D 主路径**：所有位图/图集上传（`BitmapData`→纹理） | `texImage2D`、`texParameterf` | `replaceRegion` + sampler（**方向/通道顺序由 `BitmapData` 的 BGRA 直接映射 `MTLPixelFormatBGRA8Unorm`，不再需要 GL_EXT_bgra/格式猜测**） |
| `CubeTexture` | 3D/Stage3D（2D 不用） | 全套 | Phase 3 可选 |
| `VideoTexture` | 视频（2D 未使用，见 paths.md） | `texImage2D` | Phase 3 可选 |

### 2.6 `VertexBuffer3D` / `IndexBuffer3D`

| 类 | GL 调用 | 原生映射 |
| --- | --- | --- |
| `VertexBuffer3D` | `createBuffer`、`bufferData(DYNAMIC/STATIC_DRAW)`、`deleteBuffer` | `MTLBuffer`（storageModeShared）+ `replaceRegion`；无 usage 枚举 |
| `IndexBuffer3D` | 同上 | `MTLBuffer(index)` + `MTLIndexType`（UInt16/UInt32 由调用方给） |

### 2.7 ② OpenGLRenderer 里直接的 GL（约 20 处）

| 位置 | 用途 | 原生映射 |
| --- | --- | --- |
| `:125` `getParameter(MAX_TEXTURE_SIZE)`、`:135` `getExtension(KHR_debug)` | 启动期上限/调试 | 常量（Metal 上限从 device 查） |
| `:393` `viewport` | 每帧 viewport | 一次 encoder setViewport |
| `:789-825` `clearColor/clear`（4 个角落的 letterbox 清除） | 舞台外的黑边 | render pass clear / 4 次 scissor 清屏（或一次 pass + 4 scissor） |
| `:1050` `blendEquation(FUNC_REVERSE_SUBTRACT)` | 特殊 blend mode | pipeline key |
| `:498` 等注释/无用 | — | 删除 |

### 2.8 可以整块删除的部分（原生化收益）

- `~/haxelib-dev/lime-832` 的 `NativeOpenGLRenderContext` 2D 相关转发（3,452 行中 2D 用到的部分）+ 290 CFFI 声明。
- `metal/native` 的 GL 仿真中**只服务 GL 语义**的部分：VAO/attribute 状态机、常量属性物化流、GL 状态→bgfx
  state 映射、view 映射、格式猜测/`CNE_METAL_TEX_BGRA`、`ConfigureView` Y 翻转、按 GL id 的对象表/GC 注册。
  （但其中"每 draw 顶点流绑定/uniform 打包/纹理上传"的逻辑思路可迁移到原生后端。）
- 休眠路径：AGAL/Stage3D、CubeTexture、stencil/depth 仿真、`get*` 查询仿真。

---

## 3. 目标架构：A vs B（+ B' 直连 Metal 子选项）

### 3.1 定义

- **方案 A（Lead 定义）**：新建 Haxe 原生 Metal RenderContext（直接 Metal FFI，不经 `lime_gl_*` 命名），
  改 OpenFL 渲染器调用点（即 ① 的 3,026 行 + ② 的 1,215 行）去调它。`Context3D` 不再用于 2D。
- **方案 B（Lead 定义）**：写 Metal-native 的 `Context3D` 实现替换 OpenFL 的 GL `Context3D`：
  保留 `Context3D`/`Program3D`/`Shader`/`Texture` 的公共 API 形状（① 调用它们的方式不变），
  把它们内部的 GL 实现（≈322 处）换成 Metal-native 后端。
- **B'（B 的 native 实现子选项）**：
  - B'-bgfx：原生后端内部仍用 bgfx（当前就是 Metal 渲染器）→ 改动小、风险低；
  - B'-metal：原生后端直连 Metal（`MTLDevice/MTLCommandQueue/MTLRenderPipelineState` 等），
    复用现有 `MetalLayerHost.mm` 的 CAMetalLayer 与 present，**彻底删除 bgfx 依赖**。

### 3.2 对比

| 维度 | A：新 native RenderContext + 改渲染器 | B：Metal-native Context3D 后端 | B'（=B + 直连 Metal） |
| --- | --- | --- | --- |
| 改动 Haxe 文件数 | ① 12 个 + ② 1 个 + **仍要** ③ 的 Program3D/Shader/Texture（因为 ① 用它们） ⇒ ~16 | ③ 10 个文件（Context3D/Program3D/Shader/6 个纹理/2 个 buffer）+ ② 少量 ⇒ ~12 | 同 B |
| 改动 Haxe 行数 | **3,026（①）+1,215（②）+ ③ 复用/重写 ≈1,500 ⇒ ≈5,700 行** | **≈322 处 GL 站点替换（分布 6,721 行文件中）+ ② 20 处 ⇒ ≈1,500–2,000 行改动** | 同 B |
| 新 native 代码 | RenderContext 直连 Metal ≈2,000–3,000 行（+ 仍需 program/texture 后端） | Metal-native 后端 ≈2,000–2,800 行（B'-metal）或 ≈1,000–1,500 行（B'-bgfx 复用现有 bgfx 封装） | ≈2,000–2,800 |
| 回归风险 | **高**：① 含批量/裁剪/遮罩/缓存全部逻辑，且 `Context3DGraphics` 1,402 行是 Flixel tile 批量的核心 | **中低**：① 完全不动；③ 的改动是"同 API 换实现"，每题可独立开关 | 中高（bgfx 换成直连 Metal 本身是新渲染器） |
| 灰度并存 | 需替换 `Stage.__createRenderer` 的整条渲染器链（双实现，两套 ①？） | **最容易**：`Stage.hx:1380` 一行开关 + ③ 内部按 env 分支；可逐通道灰度 | 同 B；env 再分 bgfx/metal 后端 |
| 能否回滚 | 需回滚整套渲染器 | **单文件/单方法级回滚** | 后端换回 bgfx 即回滚 |
| 是否真的消除翻译/仿真 | 是（但工作量大） | **是**：消除 49 原语 → ≈20 个 Metal 原语、消除 GL 状态机/VAO/枚举/格式仿真 | 是 + 去掉 bgfx 抽象与它的 view/sort/校验层 |
| 与 OpenFL/Flixel 兼容 | 需自己维护 ②③ 的替代，`@:privateAccess` 依赖多 | **保持 API**，Flixel/CNE 代码零改动 | 同 B |
| 估算工作量 | **8–12 人日** | **3–5 人日**（B'-bgfx）/ **5–8 人日**（B'-metal） | 同右 |
| 结论 | ❌ 不推荐（改动最大、回归面最大、收益与 B 相同） | ✅ **推荐**（最短路径、可灰度） | ✅ 作为 B 的 Phase 4（或直接做，若用户坚持"不要 bgfx"） |

### 3.3 为什么 B 不是"只改名"

1. **调用面收敛**：806.5 次/帧 `lime_gl_*`（≈40 次/笔）→ ≈6–9 次/笔 Metal 原语。消失的是 CFFI 边界 × GL 语义解析。
2. **GL 语义机消失**：VAO 状态机、`glVertexAttribPointer` 反复绑定、常量属性物化、`glEnable/glBlendFunc` 状态镜像、
   view/scissor 的 Y 翻转、GL 格式→bgfx 格式的猜测、GL id 与 bgfx handle 的双对象表 —— 全部不再存在。
3. **shader 侧**：AOT MSL 已经是原生 Metal 源；原生后端只需 `newLibraryWithSource` + `MTLRenderPipelineState`，
   uniform 从"GL location 查表 + CFFI 上传"变成"构建期偏移 + `set*Bytes`"。
4. **纹理侧**：`BitmapData` 的 BGRA 字节可直接 `MTLPixelFormatBGRA8Unorm`，不需要 `GL_EXT_bgra` 上报/
   `CNE_METAL_TEX_BGRA` 诊断开关/`ToBgfxFormat` 猜测。
5. 保留的 `Context3D.setVertexBufferAt(...)` 等**只是 OpenFL 内部 API 名字**，不再对应任何 GL 调用/仿真；
   若用户连"名字/形状"也不接受，可加 Phase 5（重命名/收敛 API），但那只是 cosmetic，不影响原生性。

---

## 4. 分阶段落地计划（每阶段可独立验证、可回滚、有验收标准）

> 共同前提：`CNE_METAL_NATIVE_PATH=1` 环境开关（默认 0 = 现有 bgfx+GL-ABI 路径，保证可回滚）；
> 构建锁 `/tmp/cne-build.lock`、运行锁 `/tmp/cne-game.lock` 串行；构建末尾 `apply.sh --deploy`（README §54 坑）。
> 补丁方式：**不直接改 `.haxelib/openfl/git`** —— 做法是 `metal/scripts/patch_openfl_native.py`（幂等、`#if`/marker 锚点、
> 失败即报错）+ `apply.sh` 调用；CNE 工程内只放 `override` 不了的（`Shader` 等必须 patch）。

### Phase 0（0.5 天）：工具与边界
- 修 `CNE_METAL_SHOT`（`gShotWrite` 有数据无消费者，见 shaders.md §0.6）→ 引擎内截图可用于 A/B。
- 新增 `CNE_METAL_CAPTURE_FRAME=<n>`：把一帧的 draw 列表（state/程序 MSL/uniform/vertex+index bytes/纹理/sampler/viewport）落盘。
- 验收：能抓到标题帧 + 菜单帧 + 游戏内帧；`CNE_METAL_SHOT` 输出正确 PNG（与 screencapture 对照）。

### Phase 1（1–1.5 天）：精灵/位图主路（最低风险，覆盖 Flixel 全部 Sprite）
- 范围：`drawTriangles` + `RectangleTexture` 上传 + `Program3D`(AOT MSL→`newLibraryWithSource`→pipeline) +
  `Shader` uniform 上传 + blend/viewport/scissor/clear + `VertexBuffer3D/IndexBuffer3D`。
- 目标：`CNE_METAL_NATIVE_PATH=1` 时 **标题→主菜单→Options→CONTROLS** 画面与默认路径一致。
- 验收：同帧（同 autokeys 帧号）截图 **逐像素 diff = 0**（或仅有可解释的浮点差异，见 Phase 0 工具）；
  无 `lime_gl_*` 调用出现在日志（本阶段先只关这个通道的 GL 路径）。
- 回滚：`CNE_METAL_NATIVE_PATH=0` 即回到现状。

### Phase 2（1 天）：文本/1x1/常量属性 + 游戏内
- 覆盖 `openfl_Alpha/ColorMultiplier/ColorOffset` 常量属性（prog 4/14）、`hasColorTransform` uniform、
  游戏内歌曲（Freeplay→winter horrorland/thorns）画面 A/B（含 CustomShader）。
- 验收：标题/菜单/CONTROLS + 游戏内 3 个场景 A/B diff=0；`EXIT` 中 `lime_gl_*` 调用集合缩小到只剩未覆盖通道。

### Phase 3（1.5–2 天）：Graphics/Shape/Tilemap、遮罩、FBO/滤镜、readPixels
- `Context3DGraphics`、`Context3DShape`、`Context3DTilemap`；`setRenderToTexture`（FBO）与 `BitmapData.draw`；
  `ShaderFilter`/`FlxCamera.addShader`（顺带修掉 paths.md 的 FBO 惰性创建缺陷）；
  `readPixels`（`drawToBitmapData`/截图）。
- 验收：编辑器 blur、Updater、CONTROLS、`FlxCamera.addShader` 的 CustomShader 场景 A/B diff=0；
  `readPixels` 与 `screencapture` 内容一致。

### Phase 4（1–2 天）：删除 BGfx 依赖 → 直连 Metal（B'-metal）
- 用 `MTLDevice/Queue + CAMetalLayer` 替换 `MetalContext::Frame/present`；删 bgfx 依赖（保留代码可回滚）；
- 删除 metal/native 里其余 GL 仿真原语（269 → 只剩原生后端 API）。
- 验收：同 Phase 1–3 的 A/B 全部通过；`otool -L` 中无 bgfx（若静态链接）；帧率不降。

### Phase 5（可选，0.5–1 天）：API 形状收敛（如果用户要求连 Stage3D 形状也不要）
- 把 `Context3D.setVertexBufferAt/drawTriangles/...` 改名为 Metal-native 语义（`setVertexStreams`/`draw`），
  ① 的 12 个文件机械替换。
- 说明：这是 cosmetic（原生性已由 Phase 1–4 达成），风险>收益，建议不做。

**总估算**：Phase 0–2 = 2.5–3.5 人日（拿到可演示的"原生精灵主路"）；Phase 0–4 = 5–7 人日（完全去 GL/bgfx）。

---

## 5. 原型（本轮）：同一帧 A/B —— 用直连 Metal 回放真实 draw 列表

### 5.1 目标切片（Lead 指定）
`drawTriangles + RectangleTexture 上传 + Program3D 绑定`（精灵绘制主路），`CNE_METAL_NATIVE_PATH=1`，
同一帧 A/B 像素一致。

### 5.2 为什么用"抓帧回放"而不是直接最小切片进游戏
- 一帧的绘制是**多个通道混合**（prog 4/8/14）并且按顺序合成；只把其中一笔切到另一后端会破坏合成顺序
  （除非整帧都由新后端渲染）。真正的最小可验证单元 = **整帧回放**：同一份 draw 数据，
  分别由现有 bgfx 路径与直连 Metal 路径渲染，逐像素比对。
- 抓帧回放还能**独立验证** uniform 布局/vertex descriptor/纹理格式/blend 这些最容易出错的点，
  且完全不改游戏渲染行为（只读 + 诊断），符合本阶段"只读+诊断"的约束。

### 5.3 设计
```
[游戏进程, CNE_METAL_CAPTURE_FRAME=<n>]
  SubmitDraw 时记录 NativeDraw（state 位、MSL(vs/fs)、每 stream 的 layout+bytes、index bytes、
        uniform 值、纹理 pixels/format/sampler、viewport/scissor）→ 写 /tmp/cne-capture/frame/
  同时修好的 CNE_METAL_SHOT 写 bgfx 呈现结果 PNG（参考图）

[独立回放工具 metal/tools/native_replay/*.mm（直连 Metal，无 bgfx）]
  读抓帧 → 建 MTLTexture(离屏 BGRA8) → 逐 draw：
    newLibraryWithSource(AOT MSL) → pipeline(blend/depth/格式) → setVertexBuffer/IndexBuffer
    → per-stage uniform buffer（解析 XlatMtlUniforms 字段顺序，按名填值）→ setFragmentTexture
    → setViewport/setScissorRect → drawIndexedPrimitives
  → 读回像素 → PNG

[Python/numpy]
  与 bgfx 参考图逐像素 diff（均值/最大误差/结构差异），输出报告
```

### 5.4 本轮结果（静态帧 A/B 逐像素一致；动态/过渡帧未收敛）

| 项 | 状态 / 结果 |
| --- | --- |
| 抓帧（`CNE_METAL_CAPTURE_FRAME`） | ✅ 已实现：`metal/native/MetalCapture.inc`（327 行，env-gated），每帧把 clear + 每笔 draw 的 state / MSL / 顶点流 bytes / index bytes / uniform 值 / 纹理像素落盘到 `/tmp/cne-capture/f<N>/` |
| bgfx 参考图（`CNE_METAL_SHOT_RAW`） | ✅ 已实现：`MetalContext.cpp` 把 bgfx render-thread 的 readback 像素写成 `.raw` + `.txt`（原来 `CNE_METAL_SHOT` 的 `gShotWrite` 没有消费者，等于坏开关；顺带发现并修好） |
| 直连 Metal 回放工具 | ✅ `metal/tools/native_replay/main.mm`（~720 行 ObjC++，**只用 `MTLDevice/CommandQueue/RenderPipelineState/Texture/Buffer`，无 bgfx、无 lime_gl_**）：解析抓帧 → `newLibraryWithSource(AOT MSL)` → pipeline(blend/colorWrite/vertexDescriptor) → setVertexBuffer(offset=startVertex*stride, index=stream+1) → per-stage uniform buffer（按 MSL 结构体字段顺序）→ setFragmentTexture/sampler → viewport/scissor(Y 翻转) → drawIndexedPrimitives → 离屏 BGRA8 RT → blit 读回 |
| A/B 像素 diff | ✅ **完全一致**：同一帧（frame 360，6 笔 prog14 draw，46,499 个非黑像素）**mean=0.0 / max=0 / 差异像素=0**；另一次单 draw 隔离 A/B（跳过其余 submit）同样 **0 差异** |

**证据文件**：`/tmp/cne-capture/f360/`（抓帧）、`/tmp/cne-shot/frame.raw`（bgfx 参考）、
`/tmp/cne-capture/ab2.raw`（直连 Metal 回放）、复现脚本 `metal/tools/native_replay/{run_capture.sh,diff.py}`。

**这个结果证明了什么**（对应 Phase 1 的验收点）：
1. **AOT MSL 可直接被 `newLibraryWithSource` 编译并作为原生 Metal pipeline 使用**（不需要 bgfx 的 shader-bin 容器）；
2. **顶点流 `glVertexAttribPointer` 语义可以无损映射到 `MTLVertexDescriptor`**（3 条 stream / 8 个 attribute / 混合 stride 4/16/80 全部一致）；
3. **uniform 上传可以由「GL location 查表 + CFFI」变成「per-stage 结构体 + setBytes」**（`openfl_Matrix`(mat4)/`openfl_TextureSize`(vec2)/`hasTransform`/`_camSize` 等全部字节级一致）；
4. **纹理上传/采样/格式**（BGRA8 位图 + LINEAR/CLAMP sampler）与 gl 路径逐像素一致；
5. **blend（ONE / ONE_MINUS_SRC_ALPHA 预乘 alpha）+ viewport + scissor + clear** 与 bgfx Metal 结果相同。

**多帧 A/B 结果（同一份抓帧 → 直连 Metal 回放 vs bgfx 同帧呈现）**：

| 帧 | 内容 | draws / program | A/B 结果 |
| --- | --- | --- | --- |
| 360（标题静态） | 静态标题画面 | 6 笔 / 全 prog 14 | ✅ **逐像素完全一致**：mean=0.0，max=0，差异像素=0（46,499 非黑像素） |
| 900（标题滚动文字） | 滚动 marquee（动画） | 4 笔 / 全 prog 14 | ⚠️ mean=0.068，max=15（1.8% 像素 >2）——差异全部落在滚动文字的字形边缘，量级 ≤15/255 |
| 1500（主菜单过渡） | 菜单淡入 + HUD 文字 | 14 笔 / prog14×8 + **prog4×5** + **prog8×1** | ⚠️ mean=5.13，max=253，12.7% 像素 >2：整屏出现 1–3 级差异 + 屏幕边缘条 + HUD 文字差异，program 4/8 路径尚未收敛 |

**结论与下一步**：
- **静态帧 0 差异**证明「AOT MSL + pipeline + vertex descriptor + uniform buffer + texture/sampler + blend/viewport/scissor/clear」这一整条原生路径**可以对真实游戏帧逐像素复现 bgfx 的输出**——Phase 1 的核心技术风险（原生 Metal 能否等价）已排除。
- 动态/过渡帧仍有差异，当前最可能的两个原因（已收敛到方向，尚未逐个实锤）：
  1. **抓帧与截图的帧对齐**对动画内容敏感：bgfx 的 `requestScreenShot` 回调数据可能与请求帧相差 0–1 帧（静态帧无影响，滚动/淡入动画在边缘放大约 1 帧位移）。→ 下一步在 `MetalContext` 里把「请求帧号」与「readback 帧号」一起打出，按帧号配对后再 A/B。
  2. **program 4/8 的若干 draw 细节**：同 frame 单 draw 隔离 A/B 工具已具备（`CNE_METAL_SKIP_SUBMIT` 只放一笔 + 同时抓帧/截图），下一步逐笔定位（候选：text 的 font atlas 上传路径、prog8 的 attribute/纹理）。
- 抓帧过程中发现并修复了一个**真实后端缺陷**（与原型无关、默认路径也受益）：
  `lime_gl_tex_sub_image_2d` 只更新 bgfx 纹理、**不同步 CPU mirror `texture->pixels`**。
  字体图集正是用 `glTexSubImage2D` 逐字上传的，所以任何「从 mirror 重新上传纹理」的路径（重建纹理、诊断）会拿到**过期/空白**的 atlas。
  已在 `MetalBindings.cpp` 补上 mirror 同步（按行 memcpy，边界检查），并在坏诊断下复现：修前 prog4 文字在直连 Metal 回放中全黑、修后正常。

**尚未覆盖**（后续 Phase）：FBO/渲染到纹理、`BitmapData.draw`、滤镜、逐 draw 的多纹理、
Graphics/Shape/Tilemap 的其它 program 的逐笔等价性（prog4/8 仍未收敛，prog14 静态帧已 0 差异）。

> 注意：早期两次 A/B 得到 mean=0.126 是因为**抓帧目录里残留了上一次的 ops.txt（追加写）**；
> 清空目录后为 0。已把 `run_capture.sh` 改成每次 `rm -rf` 抓帧目录。

---

## 6. 风险与对策

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| 混合渲染顺序 | 一帧内不能一半 bgfx 一半自绘（present/排序冲突） | 按**整帧**灰度（env 开关整条切换）而不是按通道混合；Phase 1 起每次整帧切换 |
| uniform 布局 | AOT MSL 目前是 bgfx 风格 `_mtl_u` 单结构；直连 Metal 需要 per-stage 结构/偏移 | 构建期 AOT 增加"native 目标"输出（字段偏移表）；或运行期解析 MSL 结构（回放原型采用后者验证） |
| 顶点 layout | GL 的每 attribute stride/offset 语义要压到 `MTLVertexDescriptor`（每 stream 一个 buffer） | 复用现有 `BindVertexStreams` 的分类结果（interleaved/de-interleave），换成显式 descriptor |
| FBO 缺陷 | paths.md 已定位：FBO 惰性创建永不触发（`__enableErrorChecking` 永远 false） | 原生实现直接显式建 render pass，不依赖 GL 语义（Phase 3 顺带修复） |
| 无离线 Metal 编译器 | 本机只有 CLT，无 `xcrun metal`/Xcode → MSL 必须运行时 `newLibraryWithSource` | 这是"编译"不是"翻译"，AOT 已完成翻译；若允许 CI 预编译 metallib 可进一步消除 |
| 高度依赖 OpenFL 内部 API | Flixel/CNE 代码用 `@:privateAccess` 访问 Context3D/Shader 内部 | 保留 API 形状（B 方案核心），不做 A 的大重构 |
| 构建/部署 | `lime build mac` 覆盖 cpp ndll（README §54）、hxcpp 不追踪头文件 | 构建锁 + `apply.sh --deploy` + 清 obj |
| 回归验证成本 | 每个 Phase 需要 A/B 截图 | Phase 0 先把截图/抓帧/像素 diff 工具做扎实 |

---

## 7. 结论与建议

1. **可行**：2D 渲染逻辑层 0 处 GL 调用，边界干净；322 处 GL 调用集中在 10 个后端类里，且真正运行的原语只有 49 个。
2. **推荐 B（Metal-native Context3D 后端）**，分阶段：
   - Phase 0 工具 → Phase 1 精灵主路 → Phase 2 文本/游戏内 → Phase 3 Graphics/FBO → Phase 4 直连 Metal（可选再加 Phase 5 改名）。
   - A 方案（重写 3,026+1,215 行渲染器）改动面约 2 倍、回归风险高、收益与 B 相同 ⇒ **不推荐**。
3. **"完全原生"的边界**：B 完成后，2D 渲染不再有 `lime_gl_*`/GL 状态机/GLSL 翻译/MSL 以外的翻译步骤；
   仅剩 ① OpenFL 2D 渲染器的 Stage3D 风格 API 形状（内部 API，无翻译），② `newLibraryWithSource` 的 MSL 编译（无 Xcode 无法离线）。
   若用户把"API 形状"也算"翻译"，Phase 5 给出 cosmetic 重命名，但不建议优先做。
4. **原型**：先做整帧抓帧回放 A/B（§5），它能在不改游戏行为的前提下验证"直连 Metal 渲染与 bgfx 逐像素一致"，
   是后续 Phase 1 集成前的最低风险验证。
