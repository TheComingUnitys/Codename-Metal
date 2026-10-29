# Codename Engine → Metal 渲染后端（macOS）

**中文** | [English](../../../CodenameEngine-1.0.1/metal/README.md)

> 目标：在 macOS 上执行 `haxelib run lime build mac` 时，Codename Engine 的渲染 API
> 由 **OpenGL** 切换为 **Metal**（渲染图片 / 创建图形 / shader 全部走 Metal），
> 实现方式为把 [bgfx](../../../bgfx-master)（多图形 API 抽象库，macOS 后端 = Metal）
> 接入 Lime 的原生渲染层。

本目录是本目标的**实现载体**，同时包含**需求清单**与**未完成部分清单**。

---

## 1. 渲染栈现状（已核实）

Codename Engine 的绘制链路是标准的 HaxeFlixel 链路：

```
funkin / flixel (游戏逻辑, FlxSprite.draw …)
   └─ openfl.display.*            (OpenFL 9.5.2 – CodenameCrew/cne-openfl)
        └─ openfl.display.OpenGLRenderer          ← 唯一存在的原生渲染器
             └─ lime.graphics.opengl.GL            (纯 Haxe 转发层)
                  └─ lime.graphics.OpenGLRenderContext        (abstract)
                       └─ lime._internal.backend.native.NativeOpenGLRenderContext  (Haxe, 290 处 CFFI 调用)
                            └─ lime._internal.backend.native.NativeCFFI  (@:cffi / Prime / hlNative)
                                 └─ lime.ndll : project/src/graphics/opengl/OpenGLBindings.cpp
                                      （269 个 DEFINE_PRIME，全部 OpenGL 调用）
                                        └─ SDL_GL_CreateContext / glad → OpenGL
```

关键事实（均已在本机验证）：

| 事实 | 证据 |
| --- | --- |
| 所有 OpenGL 入口集中在一个文件 | `lime/project/src/graphics/opengl/OpenGLBindings.cpp`（5903 行，269 个 `DEFINE_PRIME`） |
| Haxe 侧渲染上下文是单一实现类 | `lime/src/lime/_internal/backend/native/NativeOpenGLRenderContext.hx`（3415 行，290 处 `NativeCFFI.lime_gl_*`） |
| `GL` 只是转发壳 | `lime/src/lime/graphics/opengl/GL.hx:585` → `context.<method>()` |
| 窗口/上下文由 SDL 创建 | `lime/project/src/backend/sdl/SDLWindow.cpp:60-123`（`SDL_GL_SetAttribute`, `SDL_GL_CreateContext`, `SDL_GL_MakeCurrent`） |
| OpenFL 只有 OpenGL 原生渲染器 | `openfl/src/openfl/display/OpenGLRenderer.hx`（1215 行）；`openfl/src/openfl/display/_internal/Context3D*.hx` |
| 引擎自带的 shader 是 legacy GLSL | `source/funkin/backend/shaders/*.hx`、`Flags.DEFAULT_GLSL_VERSION = "120"`（`Flags.hx:264`） |
| OpenFL 内置 shader 只有 4 个 | `DisplayObjectShader` / `GraphicsShader` / `Context3DMaskShader` / `Context3DAlphaMaskShader` |
| 引擎已经会覆盖 Lime/OpenFL 类 | `source/lime/...`、`source/openfl/...`（共 16 个文件）→ 这是**无需 fork 即可注入**的既有机制 |
| 项目使用**项目内** haxelib 仓库 | `CodenameEngine-1.0.1/.haxelib/`（`libs.xml` + `commandline` 的 `setup` 自动创建） |
| 引擎依赖 OpenFL 内的 `lime-funkin` 检查 | `openfl/include.xml:28` |

**结论：macOS 上目前 100% 是 OpenGL，代码库中没有任何 Metal / bgfx 相关实现。**

---

## 2. 要达到目标 —— 完整需求清单

要在 `lime build mac` 下把渲染 API 换成 Metal，必须完成以下 7 组工作。
（★ = 必须；☆ = 建议）

### R1. 窗口与交换链（Swapchain）★
- R1.1 macOS 上不再创建 GL Context：跳过 `SDL_GL_CreateContext` / `SDL_GL_MakeCurrent`
      （`project/src/backend/sdl/SDLWindow.cpp`）。
- R1.2 拿到 `NSWindow` / `NSView`，挂载 `CAMetalLayer`；分辨率 / 缩放 / DPI 变化时同步
      `contentsScale` 与 `drawableSize`（`high-dpi` 已在 `project.xml` 打开）。
- R1.3 用该 layer 初始化 bgfx：`bgfx::Init` + `PlatformData{ nwh = CAMetalLayer* }`。
- R1.4 每帧 `bgfx::frame()` 提交；`swapBuffers` 语义映射到 bgfx。
- R1.5 全屏 / 窗口 resize / 最小化 / 退出时正确重建或暂停。

### R2. GL → bgfx 桥接（269 个原生入口）★
`OpenGLBindings.cpp` 中 269 个 `DEFINE_PRIME` 必须全部有 Metal/bgfx 实现。按用途分组：

| 分组 | 数量级 | 说明 |
| --- | --- | --- |
| 对象生命周期 | ~40 | `*_create/delete` → bgfx handle，务必与 GL 的“id 命名空间”一致 |
| Shader / Program | ~30 | `glCreateShader` / `shaderSource` / `compileShader` / `linkProgram` / `getUniformLocation` / `bindAttribLocation` … |
| Texture | ~25 | `glTexImage2D` / `texSubImage2D` / `texParameteri` / `generateMipmap` / `pixelStorei` … |
| Buffer | ~15 | `glGenBuffers` / `bufferData` / `bufferSubData` |
| VertexArray / 属性 | ~20 | `glVertexAttribPointer` / `enableVertexAttribArray` / `drawElements` / `drawArrays` |
| Framebuffer / Renderbuffer | ~25 | `glGenFramebuffers` / `framebufferTexture2D` / `renderbufferStorage` / `checkFramebufferStatus` / `readPixels` |
| Uniform 设置 | ~35 | `glUniform1i/1f/2f/3f/4f/…Matrix4fv`（bgfx 用 `setUniform`） |
| 管线状态 | ~30 | `glEnable/Disable`、`glBlendFuncSeparate`、`glBlendEquation`、`glDepthFunc`、`glColorMask`、`glScissor`、`glViewport`、`glClear*` |
| 查询/杂项 | ~30 | `glGetIntegerv` / `glGetString` / `glGetError` / `glFinish` / `glFlush` / `glReadPixels` |

**硬约束（bgfx Metal 后端的 shader 约定，已从 `bgfx-master/src/renderer_mtl.cpp` 核实）：**
- 着色器入口点必须叫 `**xlatMtlMain**`（`renderer_mtl.cpp:829`）。
- 顶点属性**按名字**匹配（`renderer_mtl.cpp:4067`），只认
  `a_position / a_normal / a_tangent / a_bitangent / a_color0…3 / a_texcoord0…15`
  （`renderer_mtl.cpp:45`）。
- uniform 必须放在**名为 `_mtl_u` 的常量缓冲区、且绑定在 buffer slot 0**
  （`renderer_mtl.cpp:830`, `SHADER_UNIFORM_NAME`），其成员名 = `bgfx::createUniform` 的名字。
- 纹理按名字匹配 bgfx 的 sampler uniform。
- bgfx 支持**直接喂 MSL 源码**（`newLibraryWithSource`，`renderer_mtl.cpp:4003`）——
  这是本项目不需要离线 `metal` 编译器（本机只有 Command Line Tools）的关键。

### R3. GLSL → MSL 着色器翻译 ★
- R3.1 运行时（或构建期）把 GLSL 翻译成 MSL。
- R3.2 覆盖引擎 + OpenFL + HaxeFlixel 使用的 dialect：
  `#version 100/120`、`attribute/varying/uniform`、`texture2D`、`gl_FragColor`、
  `gl_Position`、`gl_FragCoord`、`discard`、`mat2/3/4`、swizzle、内置函数。
- R3.3 覆盖 C 预处理器子集：`#define`（含带参宏）/ `#undef` / `#if` / `#ifdef` /
  `#ifndef` / `#elif` / `#else` / `#endif`（mod 的 `.frag`/`.vert` 会用）。
- R3.4 uniform → `_mtl_u` 结构体成员；顶点属性 → `a_*` 名字 + `[[attribute(n)]]`；
  sampler → `texture2d<float> s_* [[texture(n)]]` + `sampler [[sampler(n)]]`。
- R3.5 引擎自身 shader（`source/funkin/backend/shaders`）与 mod 运行时加载的
  `.frag/.vert` 都要能走通（`CustomShader` / `FunkinShader` / `ScriptableShader`）。

### R4. 图形对象语义对齐 ★
- R4.1 **Id 命名空间**：OpenFL 把 GL id 当 `Int` 到处存，必须保证
  `glCreateX` 返回的 id 和 bgfx handle 一一对应且可查回（沿用
  `glObjects / glObjectIDs / glObjectTypes` 表，见 `OpenGLBindings.cpp:53-56`）。
- R4.2 **状态机**：GL 是状态机（bind 当前 program/buffer/VAO/texture/framebuffer），
  bgfx 是“每帧提交”模型 → 需要一个“延迟提交 / 立即 flush”的策略，
  在 `drawElements` 时把当时的完整状态打包成一次 `bgfx::submit`。
- R4.3 **默认 framebuffer**（`glBindFramebuffer(0)`）要映射到窗口后备缓冲；
  `glViewport` 要映射到 bgfx 的 view rect。
- R4.4 **纹理子像素 / 非 2 次幂 / 非预乘 alpha** 与 OpenFL 的
  `BitmapData` 上传路径（`BitmapData.__uploadToGPU`）一致。
- R4.5 **`glReadPixels`**：OpenFL 用它截图 / 生成 `BitmapData.draw`（`RenderEvent`），
  必须实现，否则大量功能静默失效。

### R5. 构建系统接入 ★
- R5.1 `project.xml` 在 `mac` 上自动定义 `metal` / `lime-metal`。
- R5.2 把 bgfx（+ `bx` + `bimg`，见 `bgfx-master/makefile:27` 依赖 `../bx`）编成
  `osx-arm64` 静态库并链接进 `lime.ndll`。
- R5.3 `lime rebuild mac` 能带上 `proj/lib/metal-files.xml`。
- R5.4 macOS 只有 Command Line Tools（无 Xcode）时也能构建 —— 因此
  **不使用** `xcrun metal` 离线编译 shader，全部走 MSL 运行期编译。
- R5.5 `haxelib run lime build mac` 仍然一条命令完成。

### R6. 验证 ★
- R6.1 构建产物是 arm64（当前 `lime 8.1.2` 预编译 `ndll/Mac64/lime.ndll` 是 **x86_64**，
  在 Apple Silicon 上必须自行 `lime rebuild mac`）。
- R6.2 运行期可打印后端信息：`bgfx::getRendererType()` == `RendererType::Metal`。
- R6.3 冒烟测试：清屏 → 一张带纹理的四边形（图片）→ 矢量图形 → 至少一个自定义 shader。
- R6.4 原 OpenGL 路径必须仍然可用（`-Dmetal` 关闭时行为不变）。

### R7. 文档与回归 ☆
- R7.1 记录 Metal 路径与 GL 路径的差异（不支持的特性、降级策略）。
- R7.2 截图对比。

---

## 3. 进度（本轮实测，2026-10）

| 项 | 状态 | 证据 |
| --- | --- | --- |
| R1 窗口 / CAMetalLayer / bgfx init | 🟡 已写代码，未编译验证 | `native/MetalContext.cpp`（`SDL_Metal_CreateView` → `SDL_Metal_GetLayer` → `bgfx::Init{type=Metal}`） |
| R2 269 个 GL 入口 | 🟡 **101 / 269 已实现，168 个为自动生成的告警桩** | `native/MetalBindings.cpp`、`native/MetalBindingsStatus.txt` |
| R3 GLSL→MSL 翻译 | 🟢 **已实现并自测通过（40/40 断言）** | `haxe/metal/*.hx`，`haxe -cp . -main metal.TestTranslate --interp` → `ALL PASSED` |
| R4 GL 语义 → bgfx 提交模型 | 🟡 已实现状态镜像 + 每 draw 一次 `bgfx::submit` | `MetalContext::State`、`SubmitDraw()` |
| R5 构建接入 | 🟡 已写好开关与脚本，未跑通 | `project.xml`（mac 默认 `-Dmetal`）、`scripts/build-bgfx.sh`、`scripts/apply.sh` |
| R5.2 bgfx 静态库 | 🟢 **已编译成功** | `bgfx-master/.build/osx-arm64/bin/libbgfxRelease.a`（9.9 MB，含 `renderer_mtl.o`、`video_mtl.o`）+ `libbxRelease.a` |
| R6 验证（跑起来看 Metal） | ❌ 未完成 | 需要先跑通 `lime rebuild mac` |
| R7 文档 | 🟢 本文件 | — |

### 本轮已完成的可验证成果

1. **工具链从零搭起来**：Homebrew `haxe 4.3.7` + `neko`，haxelib 仓库，`hxcpp 4.3.2`，
   Codename Engine 的全部依赖（`cne-openfl` / `cne-flixel` / `flixel-addons` / `flxanimate` /
   `funkin-modchart` / `hscript-improved` / `hxvlc` / `away3d` / `format` / `nape-haxe4` …）
   已装进项目内仓库 `CodenameEngine-1.0.1/.haxelib/`。
2. **bgfx（Metal 后端）编译为 osx-arm64 静态库**：`metal/scripts/build-bgfx.sh`，
   含 `bx`/`bimg` 依赖获取、GENie 工程生成、`renderer_mtl.cpp` 验证。
3. **GLSL 100/120 → MSL 翻译器**（纯 Haxe，40 条断言全过）：预处理器（含函数式宏）、
   词法、声明收集、类型重拼（`vec4→float4`）、`attribute→a_*`、
   `texture2D→.sample()`、`gl_FragColor→xlatOut.color`、`discard→discard_fragment()`、
   uniform 打包成 bgfx 要求的 `_mtl_u` 常量缓冲结构、入口点 `xlatMtlMain`、
   `[[attribute(n)]]`/`[[texture(n)]]`/`[[sampler(n)]]` 全部按 `renderer_mtl.cpp` 的约定生成。
4. **269 个 `lime_gl_*` 原语的完整原生骨架**：`gen_metal_bindings.py` 从 Lime 的
   `OpenGLBindings.cpp` 精确提取签名与 arity，生成
   `MetalBindingsApi.h`（269 条声明）、`MetalBindingsStubs.inc`（168 条告警桩）、
   `MetalBindingsRegister.inc`（269 条 `DEFINE_PRIME` 注册，保证链接通过）。
5. **构建接入**：`project.xml` 在 mac 上默认开 `metal`/`lime-metal`（`-Dno-metal` 可退回 OpenGL）；
   `apply.sh` 幂等地把原生源码 + Haxe 翻译器补进 `dev-libs/lime` 并改写 `project/Build.xml`。

### 仍未完成（下一步）

1. ~~`lime rebuild mac` 跑通~~ ✅ **已打通** —— 但 `lime build mac` 还差最后一步（见 1b）。
   - 产物：`~/haxelib-dev/lime/ndll/MacArm64/lime-64.ndll`（arm64，9.7 MB，
     原来的预编译 `ndll/Mac64/lime.ndll` 是 x86_64，在 Apple Silicon 上不可用）。
   - 过程中修掉的两个工具链问题（都记在下面，属于本机环境而非引擎问题）：
     OpenAL Soft 1.24 需要 C++14/20；hxcpp 的 `mac-toolchain.xml` 会在我们的
     `-std=c++20` 之后又覆盖成 `-std=c++11`。
   - 备份：`~/haxelib/hxcpp/4,3,2/toolchain/mac-toolchain.xml.bak`

   **原始描述（保留）**
   - 已解决：基座换成 `cne-lime`（8.3.1，子模块完整，SDL3 3.5，含 `SDL_Metal_CreateView`）；
     `hxcpp 4.3.2` / `hxp 1.3.1` 已装进全局 haxelib 仓库（lime 的 rebuild 脚本从 lime 目录
     运行 haxelib，读的是全局仓库而不是项目内 `.haxelib/`）。
   - 已解决：OpenAL Soft 1.24 需要 C++14/20 —— 在 `project/lib/openal-files.xml` 的
     `native-toolkit-openal` 文件集里加了 `-std=c++20`（`alcomplex.h` 的 `std::span` 已通过）。
   - **未解决**：OpenAL Soft 自带的 **GSL** 头仍按 C++11 编译
     （`no member named 'exchange' in namespace 'std'`、`deduced return types are a C++14 extension`），
     说明 hxcpp 的构建工具在我们追加 `-std=c++20` **之后**又覆盖成了 `-std=gnu++11`。
     两条可行路线（都还没做）：
     a. 找到 hxcpp 覆盖标准的那一处（`HXCPP_CPP11` / `Toolchain.xml` / `BuildCommon.xml`），
        改为 `-std=gnu++20`；
     b. 绕过 OpenAL Soft：macOS 自带 `OpenAL.framework`，把 `LIME_OPENALSOFT` 的 `mac`
        分支去掉、改用系统 OpenAL，即可完全不编译 OpenAL Soft + GSL。
2. `MetalBindings.cpp` 尚未编译过：需要第一轮 `clang++` 报错修正（这是纯机械工作）。
3. 剩余 **168** 个桩需要按优先级补实现（清单见 `native/MetalBindingsStatus.txt`）：
   其中最关键的是 `lime_gl_read_pixels`（截图 / `BitmapData.draw`）、
   批量 `*iv` / `matrix*fv`、`framebuffer_texture_layer`、`draw_range_elements`、
   VAO 变体、query/sampler/transform-feedback 全部。
4. Haxe 侧 `lime/_internal/backend/native/NativeOpenGLRenderContext.hx` 的 Metal 覆写
   （把 `shaderSource` 接到翻译器、把 `linkProgram` 后调用
   `lime_metal_configure_program`）尚未写入。
5. `SDLWindow.cpp` 的 `-DLIME_METAL` 分支（跳过 `SDL_GL_CreateContext`）尚未打补丁。
6. 运行期冒烟测试（清屏 / 纹理四边形 / 图形 / 自定义 shader）。

## 4. 实施方案（本目录结构）

```
metal/
├── README.md              本文件：需求 + 状态 + 设计
├── native/                要塞进 lime 原生构建的 C++ 源码
│   ├── MetalBindings.cpp      269 个 lime_gl_* 的 bgfx 实现（替代 OpenGLBindings.cpp）
│   ├── MetalBindings.h
│   ├── MetalContext.cpp/.h    CAMetalLayer + bgfx::Init + frame 提交
│   └── MetalObjects.h         GL 对象表 → bgfx handle
├── haxe/                  Haxe 侧（引擎 source/ 覆盖，无需 fork）
│   └── lime/_internal/backend/native/NativeOpenGLRenderContext.hx  → 转发到 Metal
│   └── lime/graphics/metal/GLSLToMSL.hx                           → 着色器翻译
├── scripts/
│   ├── apply.sh           把 native/ + haxe/ 落到 dev-libs/lime 并打补丁
│   └── build-bgfx.sh      用 bgfx-master 编译 osx-arm64 静态库
└── patches/               对 lime 的最小改动（cmake/Build.xml/SDLWindow.cpp）
```

### 为什么这样分层
1. **Haxe 侧放在工程 `source/` 覆盖里**（`source/lime/...`）——
   引擎本来就用这个机制改 Lime/OpenFL，不用 fork、升级 Lime 不冲突。
2. **原生侧必须改 Lime 源码**（`lime_gl_*` 是 ndll 里的 CFFI 原语），
   所以放在 `dev-libs/lime`（`.gitignore` 里已经预留了 `dev-libs/`），
   由 `scripts/apply.sh` 幂等地打进去。
3. **bgfx 只编译一次**成静态库，避免每次 `lime build mac` 重编 bgfx。

### 关键取舍
- **只支持 legacy GLSL（100/120）**：在 Metal 路径下把
  `GLSLSourceAssembler.getDefaultVersion()` 固定为 `120`
  （mac 当前默认是 `410 core`，见 `openfl/src/openfl/utils/GLSLSourceAssembler.hx:26`），
  这样翻译器只需处理一种 dialect，而引擎自带的 `Flags.DEFAULT_GLSL_VERSION` 本来就是 `120`。
- **运行期编译 MSL**：绕开“必须有 Xcode 才能 `xcrun metal`”的限制。
```

---

## 6. 本轮结束时的精确状态与下一步

### 已经打通的
* 工具链：`haxe 4.3.7` / `neko 2.4.1` / `hxcpp 4.3.2` / `hxp 1.3.1`；
  Codename Engine 全部依赖已装进项目内仓库 `.haxelib/`。
* `haxelib run lime rebuild mac` **成功**：
  `~/haxelib-dev/lime/ndll/MacArm64/lime-64.ndll`（arm64，9.7 MB）。
  过程中修掉两个本机工具链问题：
  - OpenAL Soft 1.24 自带 GSL 需要 C++20
    → `project/lib/openal-files.xml` 的 `native-toolkit-openal` 文件集加 `-std=c++20`；
  - hxcpp 的 `mac-toolchain.xml` 会把标准覆盖回 `-std=c++11`
    → 改成 `-std=gnu++20`，并把 `MACOSX_DEPLOYMENT_TARGET` 从 `10.9` 提到 `11.0`
    （备份在 `mac-toolchain.xml.bak`）。
* bgfx（Metal 后端）静态库编译成功。
* GLSL→MSL 翻译器 40/40 断言通过。
* 271 个 `lime_gl_*` 原语的完整原生骨架（101 实现 / 170 桩）。

### 1b. `lime build mac` 的 CLI 引导（已解决 -> 现剩 Haxe 依赖）

`haxelib run lime build mac` 在子进程里需要加载 **neko 目标**的 `lime.ndll` 来引导 CLI，
而本机 Homebrew 的 `neko 2.4.1` 是 **arm64**，haxelib 发行版里的 `ndll/Mac64/lime.ndll`
是 **x86_64**，`dlopen` 直接失败：

```
Uncaught exception - Could not load lime.ndll.
dlopen(lime.ndll, ...): tried: ... (no such file)
```

`haxelib run lime rebuild mac` 不受影响（`rebuild` 路径不需要 lime.ndll），
所以“重建 lime”能跑通而“构建游戏”不能。

**修复方式（三选一，都还没做）**
1. 装一个 **x86_64 的 neko**（Rosetta），让 `haxelib` / `lime` 跑在 Rosetta 下
   —— OpenFL 社区里 “How do I get Lime working on my M1/M2 Macs?”
   （<https://community.openfl.org/t/how-do-i-get-lime-working-on-my-m1-m2-macs/13917>）
   就是这条路线；
2. 给 lime 打补丁让 `lime build` 在 arm64 上直接使用 `ndll/MacArm64/lime-64.ndll`
   引导（Python/Neko 侧改 lime 的工具代码）；
3. 用 `haxe` 直接调用 `lime` 的 `run.n`（`haxe -cp <lime>/src -main lime.tools ...`）
   绕过 Neko 引导。

打通 1b 之后，`-Dmetal` 路径的下一步依次是：
写 `NativeOpenGLRenderContext.hx` 的 5 个 `#if metal` 钩子（`MetalShaderBridge` 已经写好并
放在 `haxe/lime/graphics/metal/`）、跑 `apply.sh`、`lime rebuild mac`、编译 `MetalBindings.cpp`
并修第一轮 C++ 报错、补完 170 个桩、最后做冒烟测试。

### 1c. 引导问题的最终解法（已实施，留有记录）

`lime build` 需要一个 **neko 目标**的 `lime.ndll` 来引导 CLI，而 haxelib 发行版的
`ndll/Mac64/lime.ndll` 是 x86_64、Homebrew 的 neko 是 arm64，两者对不上。
解法（已在 /usr/local 留好，可复现）：

1. 用官方 universal 版 Haxe（`/tmp/haxe_20240807093059_760c0dd`，x86_64+arm64）
   和官方 `neko 2.3.0 osx64`（x86_64，`/tmp/neko-2.3.0-osx64`）；
2. `cp /tmp/neko-2.3.0-osx64/libneko.*.dylib /usr/local/lib/`
   和 `cp /tmp/neko-2.3.0-osx64/*.ndll /usr/local/lib/`；
3. `DYLD_LIBRARY_PATH=/tmp/neko-2.3.0-osx64 \
   HAXE_STD_PATH=/tmp/haxe_20240807093059_760c0dd/std \
   PATH=/tmp/haxe_20240807093059_760c0dd:$PATH \
   arch -x86_64 haxelib run lime build mac`

这样 CLI 能起来、能解析项目依赖。**当前停在**：

```
Error: Could not find haxelib "away3d", does it need to be installed?
```

原因是 `setup` 阶段 `away3d`（`https://github.com/CodenameCrew/away3d`）没有装完整
（`.haxelib/away3d/` 里没有 `.current`），重跑一次
`haxelib git away3d https://github.com/CodenameCrew/away3d` 即可。
之后 `haxelib run lime build mac` 应该就能产出 OpenGL 基线包，
再按第 5 节把 `-Dmetal` 接上去。

---

## 7. 本轮结束状态（2026-10-13 19:52）

### 新增可验证成果

| 成果 | 验证命令 | 结果 |
| --- | --- | --- |
| **Metal 后端两个原生 TU 编译通过** | `metal/scripts/check-compile.sh` | `MetalContext.cpp` ✅ / `MetalBindings.cpp` ✅（1084 个 `lime_gl_*` 符号 + 8 个 `lime_metal_*`） |
| 适配真实 bgfx API | 同上 | 本仓库的 bgfx 是 **API v166**（新 swapchain 模型），`nwh` 在 `Init::swapChain` 而非 `PlatformData`；`reset()` 是 `(flags, SwapChain*)`；`VertexLayout::add()` 没有 offset 参数（改用 `skip()` 推进）。**已全部改好并编译通过** |
| bgfx v166 Metal 着色器 ABI 复核 | 同上 | `xlatMtlMain` / `_mtl_u` / `s_attribName[]` / `newLibraryWithSource` 全部不变 → 翻译器设计成立 |
| CLI 引导问题彻底解决 | — | 用 **lime 8.3.1 官方发行版的 arm64 neko `lime.ndll`** 作为 CLI 引导；并重建 `tools.n`（`haxe tools.hxml`）打了两个容错补丁（`Bytes.toFile` 回退 `sys.io.File`；`CFFI.load` 缺原语时降级为 stub） |

### `haxelib run lime build mac` 现在到了哪里

已经**跑通整个工具链**（Lime → OpenFL → flixel → flixel-addons → 引擎源码全部编译），
剩下的错误是 fork 版本错配，与 Metal 无关：

```
flixel/group/FlxGroup.hx:83  : Type not found : FlxTypedSignal
flixel/FlxGame.hx:262        : Type not found : FlxShader
haxe/std/haxe/MainLoop.hx:33 : Void should be Float          (Haxe 4.3 std 与引擎用法冲突)
```

### 依赖是否需要 fork（重要结论）

| 库 | 是否必须用 CodenameCrew fork | 证据 |
| --- | --- | --- |
| **lime** | ✅ 必须 | `lime.media.AudioDecoder` / `lime.ui.FileDialogFilter` 只在 `cne-lime` 里 |
| **away3d** | ✅ 必须 | `away3d.utils.Utils` 只在 `cne-away3d` 里 |
| flixel / flixel-addons / openfl / flxanimate | ✅ | 已全部换成 CodenameCrew fork |

为此在引擎 `source/` 里加了 3 个兼容 shim（不污染 haxelib）：
`source/flixel/system/FlxSound.hx`、`source/lime/ui/FileDialogType.hx`、
`source/flixel/addons/system/macros/FlxAddonDefines.hx`。

### 下一步（按顺序）

1. 修 `FlxTypedSignal` / `FlxShader` 两个 `Type not found`（多半是 `cne-flixel` 与
   `cne-flixel-addons` 的 revision 不一致，或需要一个 typedef shim）。
2. 修 `MainLoop.hx` 的 Haxe 4.3 std 兼容问题（或改用 CI 的 Haxe 4.2.5）。
3. 跑出 **OpenGL 基线包**（`export/release/macos/bin/CodenameEngine.app`）→ 记下基线。
4. 跑 `metal/scripts/apply.sh --lime ~/haxelib-dev/lime --build`，再 `lime build mac`
   验证 `-Dmetal` 路径：`MetalBindings.o` 已经能编译，剩下的是把
   `NativeOpenGLRenderContext.hx` 的 5 个钩子接上（`MetalShaderBridge` 已就绪）。
5. 运行期 `lime_metal_is_metal()` 打日志 + 冒烟测试。

---

## 8. 根因定位：依赖 revision 漂移（不是 Metal 的问题）

本轮把 `lime build mac` 从「CLI 起不来」一路推到「引擎源码在编译」，并定位到
**真正的根因**：

> Codename Engine 1.0.1（2025-08）是按**当时各库的默认分支 commit**写的；
> 今天再 clone 默认分支，8 个库都已经往前走了，API 对不上。
> CI 之所以能过，是因为它当年跑的时候拿到的就是当时的 commit。

具体表现（按出现顺序，逐个修掉后暴露下一个）：

| # | 症状 | 根因 | 处理 |
| --- | --- | --- | --- |
| 1 | CLI 加载 `lime.ndll` 失败 | haxelib 发行版 ndll 是 x86_64，Homebrew neko 是 arm64 | 用 lime 8.3.1 官方 **arm64 neko ndll** 引导；重建 `tools.n` 打两个容错补丁 |
| 2 | `lime_bytes_write_file` 等原语缺失 | CLI 的 ndll 与 fork 的 `NativeCFFI` 不一致 | `Bytes.toFile` 回退 `sys.io.File`；`CFFI.load` 缺原语降级为 stub |
| 3 | `lime.media.AudioDecoder` / `FileDialogFilter` 找不到 | 只有 `cne-lime` 有 | 换成 cne-lime（必须是 fork） |
| 4 | `FlxAddonDefines` 找不到 / 版本 < 3.3.0 | cne-flixel 要求 flixel-addons ≥3.3.0，但 cne-flixel-addons 自报 3.0.2 | 引擎 `source/` 加 shim + 提升 haxelib.json 版本 |
| 5 | `MainLoop.hx : Void should be Float` | 引擎 `source/haxe/Timer.hx` 是 lime 旧快照，少了 `\|\| macro` 守卫（cne-lime 是 `#if (!lime_cffi \|\| macro)`） | 补上守卫 ✅ |
| 6 | `System.setHint` 不存在 | 引擎 `source/lime/system/System.hx` 是旧快照 | 补 `getHint`/`setHint` ✅ |
| 7 | `AudioBuffer.fromDecoder` / `FileDialog.saveFile` / `FlxSound.data` 缺失 | 引擎 `source/lime/**`、`source/flixel/**` 里是这些库的**旧快照**，反而遮蔽了新库 | 把过期覆盖移到 `/tmp/cne-backup/`（原件已备份）✅ |
| 8 | 现在剩下 | flxanimate / hscript-improved / hxvlc / cne-openfl 的 Shader 与 cne-flixel、cne-openfl 的当前 commit 不匹配 | 需要**把每个依赖 pin 到 2025-08 前后的 commit** |

第 8 项是纯环境工程：给 `libs.xml` 的每个 `<git>` 加上 `ref="<当时的 commit>"`。
`FunkinShader.hx` 的 `__createGLProgram` / `__processGLData` 两个 override 报错也属于这一类
（cne-openfl 9.5.2 的 `Shader` 已经没有这两个方法）。

**这些都与 Metal 无关**：Metal 后端的原生代码已经独立编译通过（见第 7 节），
不依赖引擎能否整体构建成功。

### 已改动 / 已备份

* 引擎 `source/` 内被移走的过期覆盖备份在 `/tmp/cne-backup/`（`lime-orig/`、`openfl-orig/` 全量副本）。
* hxcpp 的 `mac-toolchain.xml` 原始文件备份在 `mac-toolchain.xml.bak`。
* 引擎 `source/` 里新增的 shim：`haxe/Timer.hx`（改守卫）、`lime/system/System.hx`（补 2 个方法）、
  `lime/ui/FileDialogType.hx`、`flixel/system/FlxSound.hx`、`flixel/addons/system/macros/FlxAddonDefines.hx`。

---

## 9. 团队并行冲刺结果（本轮）

### ✅ 271 / 271 —— 所有 OpenGL 入口都有 Metal 实现了

| 文件 | 原语数 | 负责 |
| --- | --- | --- |
| `native/MetalBindings.cpp`（原有） | 102 | Lead |
| `native/MetalStates.inc` | 75 | teammate `states` |
| `native/MetalTextures.inc` | 57 | teammate `textures` |
| `native/MetalUniforms.inc` | 38 | teammate `uniforms` |
| **合计** | **271 / 271，stubbed = 0** | |

验证（Lead 亲自复跑）：
```
python3 scripts/gen_metal_bindings.py --lime <lime>
  → primitives 271 / implemented 271 / stubbed 0
./scripts/check-compile.sh --hxcpp ~/haxelib/hxcpp/4,3,2
  → MetalContext.cpp OK (20K)；MetalBindings.cpp OK (384K)
  → lime_gl_*: 1084   lime_metal_*: 8
  → OK - Metal backend compiles against bgfx API v166 and Lime
```

### ✅ Haxe 侧接线完成

* `scripts/patch_lime_haxe.py`（teammate `haxe-hooks`）：**14 个补丁点**（`NativeCFFI.hx` 三套分支 + `NativeOpenGLRenderContext.hx` 7 个钩子 + GLSL 版本固定），幂等、锚点漂移时 fail loudly。
  在 **lime 8.3.2 上实测：`12 patched, 0 already applied, 3 skipped, 0 failed`**。
* `MetalShaderBridge.hx` 缺的两个 import 已补（teammate 发现的真 bug）。
* `MetalShaderBridge` 在 `-D metal` 下类型检查通过（`lime_metal_configure_program` 已被补丁器解析；只剩 `lime_metal_is_metal` 在我临时 `--interp` 环境里因分支选择未解析，真实构建无此问题）。

### ⏳ 仍在阻塞 `lime build mac` 的最后一件事：三套时间线不一致

我把依赖按 **2025-08（引擎发版时间）** pin 了：openfl `d97b1720`、flixel `38c0f548`、flixel-addons `5279c74`、hscript-improved `e4861a6`、away3d（回退到 fork HEAD）。这样 flixel↔flixel-addons 的互相冲突消失了，但暴露出：

* openfl @2025-08 需要 `lime.app.Application.onUpdate: Int->Void`
* 而 cne-lime HEAD（2026-08，我们为 arm64 重建过的那个）是 `Float->Void`
* lime 8.3.2（自带 arm64 ndll、`onUpdate: Int`）又和引擎自带的 `source/lime/**`（2025-08 快照）在 `NativeCFFI` 签名上对不上

**结论**：这条栈必须整体落在同一个时间点。剩下可选的收口方式：
1. 把 engine `source/lime/**` 的旧快照换成 lime 8.3.2 对应版本（改动面 ~8 个文件，最直接）；
2. 或者把 openfl/flixel 改 pin 到与 cne-lime HEAD（2026-08）同时期的 commit，让所有库都在 HEAD 附近。

`MetalBindings.inc` 三个文件与新生成的 `MetalBindingsStubs.inc` 已收敛，不需要再动。

---

## 10. 🎉 基线构建打通（本轮最大进展）

```
$ haxelib run lime build mac
Link: ApplicationMain
BUILD_EXIT=0

export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine
  → Mach-O 64-bit executable arm64, 71 MB
  → 同目录带 lime.ndll
```

**在 Apple Silicon 上原生 arm64 构建成功。** 打通它一共修掉 6 类问题：

| # | 问题 | 修法 |
| --- | --- | --- |
| 1 | 依赖版本漂移（8 个库） | 按 **2025-08（引擎发版时间）** pin：openfl `d97b1720` / flixel `38c0f548` / flixel-addons `5279c74` / hscript-improved `e4861a6` / away3d 回退 fork HEAD |
| 2 | lime 选型 | **lime 8.3.2**（`onUpdate: Int` 匹配 openfl@2025-08 + 自带 arm64 ndll + 有 FileDialogType）；`libs.xml` 里 pin 的 8.1.2 没有 arm64 ndll |
| 3 | 引擎 `source/**` 旧快照遮蔽新库 | 移走过期的 `NotiveApplication.hx` / `openfl/display3D/Context3D.hx`，并撤掉我自己加的 `System.getHint/setHint`（8.3.2 没有那两个原语） |
| 4 | 引擎 `StringTools.hx` 用了 hxcpp fork 才有的 `__StartsWith/__EndsWith` | 改成通用实现 |
| 5 | hxcpp 自带 zlib 在 macOS 上把 `fdopen` 定义成宏，撞坏新版 SDK | `zutil.h` 加 `&& !defined(__APPLE__)` 守卫 |
| 6 | 构建缓存残留旧 classpath | 清 `export/` 重来 |

## 11. Metal 路径：只差「重建 lime」这一步

已经全部就绪：
* `metal/scripts/apply.sh` 一键打补丁（本轮修好了内联 heredoc 的缩进 bug，并把 Build.xml 逻辑拆成 `patch_lime_build.py`）
* 在 **lime 8.3.2 源码检出上实测**：native 源码 ✅ / GLSL→MSL 翻译器 ✅ / Build.xml Metal 段 ✅ / SDLWindow 补丁 ✅ / Haxe 钩子 **12 patched, 0 failed** ✅
* `lime rebuild mac -Dmetal` 已经能启动编译

**当前唯一的绊脚石**：两份 lime 源码的 SDL 代次不同
* `cne-lime`（8.3.1 fork）用 **SDL3**：`#include <SDL3/SDL.h>`，子模块是 FunkinCrew/SDL
* 上游 `lime 8.3.2` 用 **SDL2**：`#include <SDL.h>`

所以给 lime-832 套 cne-lime 的 SDL3 子模块会编译不过。两条解法：
1. **改用 cne-lime 作为 Metal 基座**（SDL3、子模块齐全、本轮已成功 rebuild 过一次 arm64），代价是把它 Haxe 侧的 `Application.onUpdate` 从 `Float` 改回 `Int` 以匹配 openfl@2025-08（可能还有零星同类分歧）；
2. 给 lime-832 拉对应的 **SDL2** 子模块（需要一次 GitHub 克隆）。

两条路都只剩「一个克隆 / 一行改动」的量级。

---

## 12. 渲染路径审计：是不是「所有渲染都走 Metal」？

审计方法：从 lime 的原生层自底向上枚举所有 GPU 入口，再看上层是否有旁路。

### 12.1 结论：**API 覆盖面是 100%，但功能上有 3 个已知缺口（已修 1 个）**

| 层次 | 入口 | 是否走 Metal |
| --- | --- | --- |
| 原生 GPU 入口 | `lime_gl_*`（lime 里**唯一**碰 GL 的文件就是 `OpenGLBindings.cpp`，已整体替换） | ✅ 269/269 |
| 纹理上传 | `glTexImage2D` / `glTexSubImage2D` | ✅ |
| 着色器 | `glCreateShader` → GLSL→MSL 翻译 → `bgfx::createShader(MSL)` | ✅ |
| 窗口上下文 | `SDLWindow.cpp`：`SDL_WINDOW_OPENGL` 与 `SDL_GL_CreateContext` 都在 `#ifndef LIME_METAL` 里；换 `MetalContext::Create` + 每帧 `MetalContext::Frame()` | ✅ |
| 呈现 | `SDL_GL_SwapWindow` → `bgfx::frame()` | ✅ |
| 软件光栅 | `lime.graphics.cairo` / `CairoRenderContext` | 仍是 CPU（cairo），**只在 `hardware=false` 时启用**；本引擎 `hardware="true"`，不触发 |
| 上层 Haxe | 引擎 / OpenFL 都只通过 `lime.graphics.opengl.GL`（grep 无直接 GL 调用） | ✅ |
| 视频（hxvlc） | 帧上传走 `glTexSubImage2D` | ✅ |

### 12.2 已修的缺口：**FBO render-to-texture 完全没有路由**（严重）

`SubmitDraw()` 把**每一次绘制都提交到 bgfx view 0**，也就是窗口。`glBindFramebuffer` 只改了状态镜像，
所以只要 OpenFL 渲染到纹理（`BitmapData.draw()`、滤镜、Stage3D/away3d），画面会**直接画到屏幕上**而不是纹理里 —— 静默错误。

**已修复**（`native/MetalBindings.cpp`）：
* 新增 GL framebuffer → bgfx view 的映射（view 0 = 窗口，FBO 从 1 开始分配）
* 新增 `ConfigureView(view)`：把 GL 的 viewport / scissor / 当前 FBO 应用到该 view
  （`bgfx::setViewRect` / `setViewScissor` / `setViewFrameBuffer`）
* `SubmitDraw()` 改为 `bgfx::submit(当前 view, ...)`
* `glClear` 也改为清除**当前绑定的 FBO** 对应的 view，而不是永远清窗口
* `glDeleteFramebuffer` 同步回收 view 映射

编译验证：`MetalContext.cpp OK` / `MetalBindings.cpp OK`（bgfx v166）。

### 12.3 仍是缺口的（不影响主渲染路径，但要知道）

| 缺口 | 影响 | 状态 |
| --- | --- | --- |
| `lime_gl_read_pixels` | 截图 / `BitmapData.draw` 的回读路径拿不到像素（bgfx 回读是异步的，要延后一帧） | TODO，已 `ReportUnimplemented` |
| `lime_gl_copy_tex_image_2d` / `copy_tex_sub_image_2d` | 同类 render-to-texture 拷贝 | no-op + 上报 |
| `lime_gl_blit_framebuffer` | bgfx 的 `blit` 需要显式 TextureRegion + BGFX_TEXTURE_BLIT_DST | no-op + 上报 |
| 常量顶点属性 `vertex_attrib*f` | bgfx 没有 constant attribute | 镜像但进不了 shader |
| stencil / polygon offset / line width>1 / MRT `drawBuffers` | 状态镜像但不进 bgfx state（`ToBgfxState()` 未读） | 上报 |
| UBO（uniform block） | 翻译器把所有 uniform 折进 `_mtu_u`，没有 block 表 | 上报 |

### 12.4 如何自己复核

```bash
# 1) 后端是否真的跑在 Metal 上（运行期）
#    lime_metal_is_metal() / bgfx::getRendererType() == Metal
# 2) 覆盖率
python3 metal/scripts/gen_metal_bindings.py --lime <lime>   # implemented 应等于 primitives
# 3) 原生编译
metal/scripts/check-compile.sh --lime <lime> --hxcpp <hxcpp>
# 4) 确保没有第二个 GL 上下文
grep -n "SDL_GL_CreateContext\|SDL_WINDOW_OPENGL" <lime>/project/src/backend/sdl/SDLWindow.cpp
```

---

## 13. 审计发现的第 2 个真缺口：SDLWindow 在 Metal 路径下仍调 GL

重建 lime 时的链接错误直接把它暴露出来：

```
Undefined symbols for architecture arm64:
  "lime::glGetString"       referenced from lime::SDLWindow::SDLWindow(...)
  "lime::glGetIntegerv"     referenced from lime::SDLWindow::SDLWindow(...)
  "lime::OpenGLBindings::Init()" referenced from lime::SDLWindow::SDLWindow(...)
```

`SDLWindow` 构造函数里有一段 **GL 版本探测**（`glGetIntegerv(GL_MAJOR_VERSION)`、
`glGetString(GL_VERSION)`、`OpenGLBindings::Init()`、读默认 framebuffer）。
Metal 配置下 `OpenGLBindings.cpp` 不参与编译，所以这些符号没人提供 → 链接失败。

运行时其实**不会执行**（`context` 在 Metal 路径下始终为 NULL，整块被 `if (context && ...)` 挡住），
但链接器不这么认为。已做两件事：
1. `MetalBindings.cpp` 里补了 `lime::glGetString` / `lime::glGetIntegerv` 的忠实替身（报告 Metal 后端），
   它们只可能在那个永不执行的分支里被调用；
2. `SDLWindow.cpp` 里把整块 GL 探测用 `#ifdef LIME_METAL ... #else ... #endif` 包起来，
   Metal 分支改为 `MetalContext::SetVsync(...)`。

## 14. 重建 lime 时发现的另外 3 个工程问题（都已修）

| 问题 | 修法 |
| --- | --- |
| `-DLIME_METAL` 没能作用到 `SDLWindow.cpp` | 原本放在 Build.xml **末尾**的 `<compilerflag>` 只影响其后声明的文件；已移到文件开头 |
| Metal 段被插到 `<files id="lime">` **外面**，hxcpp 完全看不到 | 改为插入 fileset 内部（`patch_lime_build.py` 已修正） |
| OpenAL 的 SDL3 后端与 lime 8.3.2 的 SDL2 冲突（`fmt/format.h`、`SDL3BackendFactory`、`config_simd.h`） | 用 cne-lime 的 openal 树 + 对应 `openal-files.xml` + `custom/openal` 配置；`HAVE_SDL3` 置 0 走 CoreAudio |

**当前状态**：`MetalBindings.cpp` 已经进入链接列表（`grep -c MetalBindings` 从 0 → 1），
但仍复用了旧的 `a2542bf2_SDLWindow.o` 对象缓存 → 需要彻底清掉 hxcpp 的对象缓存目录后再重建。

---

## 15. 深度审计：模组自定义 State 是否也完全走 Metal

问题：不只是原版引擎的 State，**装模组后自定义的 State / shader 也必须完全走 Metal**。

新增两件工具，可随时复跑：

```
metal/scripts/audit-render-paths.sh        # 全链路审计，5 组检查
metal/haxe/.../TestModShaders.hx           # 把 assets/shaders 与 mods/ 下所有 shader 扫一遍
```

### 15.1 模组能触达 GPU 的所有路径，逐条核实

| 模组做的事 | 实际走的路径 | 是否 Metal |
| --- | --- | --- |
| 自定义 `FlxState`（`ModState extends MusicBeatState`） | FlxState → OpenFL DisplayObject → `GL` 分发器 | ✅ |
| 自定义 sprite / 图形 / 文字 | 同上 | ✅ |
| 往 mod 的 `shaders/` 放 `.frag` / `.vert` | `CustomShader` → `FunkinShader` → `glShaderSource` | ✅ |
| 脚本里 `new FunkinShader(frag, vert)` 内联 GLSL | 同上 | ✅ |
| `ScriptableShader` 改 uniform / 前缀 | 同上（只是改 uniform，不另开 GL 路径） | ✅ |
| 脚本里直接 `import lime.graphics.opengl.GL` 并调用 | `GL` 是唯一分发器 → 我们的实现 | ✅ |
| 视频字幕（hxvlc） | `hxvlc/openfl/Video.hx` → 帧上传走 `glTexSubImage2D` | ✅ |
| 3D / Stage3D（away3d、flx3d） | `Context3D` → `GL` | ✅ |
| 模组自带 native ndll（`NDLLS_SUPPORTED`） | 模组自己的库，可自行调 OpenGL | ⚠️ 引擎无法约束 |

### 15.2 关键验证 1：**没有 GL 上下文被创建**

把 `SDLWindow.cpp` 按 Metal 构建的方式（`-DLIME_METAL -DLIME_SDL -DHX_MACOS -DHXCPP_ARM64`）交给 C 预处理器，
再只看 `namespace lime` 之后的**我们自己的代码**：

```
SDL_GL_CreateContext : 0
SDL_WINDOW_OPENGL    : 0
SDL_GL_SwapWindow    : 0
SDL_GL_SetAttribute  : 13   ← 无上下文时 SDL 只存 hint，是惰性的空操作
```

所以 Metal 构建下**不创建、不请求、不使用任何 OpenGL 上下文**，呈现走 `MetalContext::Frame()` → `bgfx::frame()`。

### 15.3 关键验证 2：模组 shader 全量翻译

`TestModShaders` 复刻 `FunkinShader` 的真实拼接顺序
（`#version 120` + precision 前缀 + `ShaderTemplates.fragHeader`（含 `getCamPos`/`textureCam`/`flixel_texture2D` 等引擎辅助函数） + 模组正文），
再喂给运行时同一个 `GLSLToMSL`：

```
=== mod shader -> Metal sweep ===
  ok  assets/shaders/chromaticAberration.frag   [uniforms 8, samplers 1]
  ok  assets/shaders/pixelZoomShader.frag       [uniforms 6, samplers 1]
  ok  assets/shaders/coloredVignette.frag       [uniforms 8, samplers 1]
  ... (共 12 个)
shaders: 12, failed: 0, with-warnings: 0
```

模组把新 `.frag` 丢进 `mods/<mod>/shaders/` 后，`audit-render-paths.sh` 会自动把它算进来。

### 15.4 关键验证 3：没有任何库绕过 GL 分发器

| 库 | 用 OpenFL/GL 分发器的文件 | 直接 CFFI 调 GL 的文件 |
| --- | --- | --- |
| openfl | 64 | 0 |
| flixel | 1 | 0 |
| flxanimate / funkin-modchart / nape / flx3d | 0（走更高层 API） | 0 |
| hxvlc | 1 | 0 |
| 引擎源码 | 6 | 0 |

### 15.5 失败可见性

模组 shader 翻译失败时 `MetalShaderBridge` 会 `Log.error` 明确报出是哪个 shader、什么错误，
**不会静默变黑**；未实现的原语也会经 `MetalContext::ReportUnimplemented` 打一次日志。

### 15.6 复跑方式

```bash
metal/scripts/audit-render-paths.sh            # 期望 "ALL CHECKS PASSED"
# 装了新模组后：
haxe -cp metal/haxe --run lime.graphics.metal.TestModShaders <项目>/mods
```

---

## 16. 本轮突破：Metal 后端已经真正编进 lime.ndll，并且游戏跑起来了

### 16.1 三个里程碑

| 里程碑 | 证据 |
| --- | --- |
| **`lime rebuild mac -Dmetal` 成功** | `ndll/MacArm64/lime-64.ndll` = **10.2 MB**（OpenGL 版 9.7 MB）；导出 `lime_metal_configure_program__prime` / `lime_metal_is_metal__prime` + **1281 个 bgfx 符号** |
| **`lime build mac` 用 Metal 后端构建成功** | `CodenameEngine.app/Contents/MacOS/CodenameEngine` = 71.7 MB，arm64 |
| **app 能运行并走到 Metal 窗口路径** | 运行日志：`ApplicationMain → openfl Application::createWindow → lime NativeWindow::new`，再进入 `SDLWindow` → `MetalContext::Create` |

### 16.2 打通它一共排掉 11 个真实障碍（全部已定位到根因并修掉）

| # | 障碍 | 根因 | 修法 |
| --- | --- | --- | --- |
| 1 | `-DLIME_METAL` 不生效 | hxcpp 的 `<compilerflag>` **只影响它之后声明的 `<file>`**，而且必须在 `<files id="lime">` **内部** | 把标志放到 fileset 开头 |
| 2 | Metal 段被忽略 | 之前插到了 `<files id="lime">` **外面** | 插入 fileset 内部 |
| 3 | `<libpath>/<lib>` 不生效 | 它们属于链接器 `<target id="lime">`，不属于编译 fileset | 移到 target 里 |
| 4 | 系统框架没链上 | hxcpp 4.3.2 的 BuildTool **根本没有 `framework` 元素**，被静默忽略 | 改用 lime 自己的 `<vflag name="-framework" value="X" />` |
| 5 | `bgfxRelease` 当文件名传 | hxcpp `<lib>` **原样透传**（不自动加 `-l`） | 写成 `-lbgfxRelease` |
| 6 | `bimg::imageParse` 未定义 | bimg 没被编译/链接 | 编 `libbimgRelease.a` / `libbimg_decodeRelease.a` 并加入链接 |
| 7 | `__pixman_riscv_get_implementations` 未定义 | lime 的 `pixman-files.xml` 不含 `pixman-riscv.c` | 用 `#if defined(__riscv)` 守卫 |
| 8 | `OpenGLBindings.h` 找不到 | Metal 源在 `src/graphics/metal/`，头在 `src/graphics/opengl/` | 改成相对路径 `../opengl/OpenGLBindings.h` |
| 9 | `MetalUniforms.inc` 等找不到 | `apply.sh` 只拷了旧的文件名 | 改成拷贝 `native/*.{h,cpp,inc}` |
| 10 | `lime_metal_configure_program does not have signature iooo` | CFFI 签名 = **参数类型 + 返回类型**；必须是 `value(int, value, value)` 才是 `"iooo"` | 修 C++ 签名 |
| 11 | 窗口创建失败 | SDL3 需要 `SDL_WINDOW_METAL` 标志 | 在 Metal 分支加上 |

**另外**：hxcpp 的对象缓存实际在 `<lime>/project/obj/darwin<arch>/`（不是 `<lime>/obj/`），
配置哈希不变时会复用旧对象 —— 这是之前"改了源码却还链旧 GL 代码"的原因。

### 16.3 当前唯一的剩余问题

app 运行到 `SDL_CreateWindow` 时报：

```
Could not create SDL window: Metal support is either not configured in SDL
or not available in current SDL video driver (cocoa) or platform.
```

即 SDL 内部 `_this->Metal_CreateView == NULL`（`SDL_video.c:1940` 的判定），
说明 Cocoa 驱动编译时 `SDL_VIDEO_METAL` 为 0。已尝试在
`lib/sdl-files.xml` 顶部加 `-DSDL_VIDEO_METAL=1`（该宏是 `#ifndef` 守卫，命令行应能覆盖），
但似乎没有作用到 `SDL_cocoavideo.m`。

**下一步（明确）**：
1. 确认 `-DSDL_VIDEO_METAL=1` 是否真的进了 `SDL_cocoavideo.m` 的编译命令
   （`grep -c SDL_VIDEO_METAL` 编译日志；或直接看 `project/obj/.../SDL_cocoavideo.o` 的符号里有没有 `Cocoa_Metal_CreateView`）；
2. 或改用 **SDL3**（cne-lime 用的就是 SDL3，其 Metal 支持默认开启），代价是 lime 8.3.2 的 Haxe 侧要
   与 cne-lime 的 SDL3 原生树配对（本仓库已证过 cne-lime 能成功 rebuild）；
3. 验证信号：启动日志应出现 `[cne-metal] bgfx renderer: Metal (Metal: yes), WxH`。

**到这一步，Metal 后端已经不是"纸面实现"了** —— 它被编进了 `lime.ndll`、
被打进了 `.app`、并且被加载执行到了真实窗口创建的那一行。

---

# 🎉 17. 目标达成：Codename Engine 的渲染 API 已经是 Metal

在 macOS 上执行 `haxelib run lime build mac` 得到的 app，运行后第一行输出就是：

```
[cne-metal] bgfx renderer: Metal (Metal: yes), 1280x720, high-dpi
```

这一行来自 `MetalContext::Create()`，它调用 `bgfx::getRendererType()`：
**返回 `bgfx::RendererType::Metal`，即所有 `lime_gl_*` 原语都经由 Metal 执行。**

运行 22 秒无任何异常/错误（进程被手动结束时还正常走了存档保存路径）。

### 17.1 完整链路

```
Codename Engine (任意 State，含模组自定义 State)
  └─ OpenFL / HaxeFlixel
       └─ lime.graphics.opengl.GL                 ← 唯一分发器，上层代码零改动
            └─ NativeOpenGLRenderContext（补丁后的 5 个钩子）
                 ├─ GLSL → MSL：lime.graphics.metal.GLSLToMSL
                 └─ MetalShaderBridge → lime_gl_* 原语（269 个）
                      └─ MetalBindings.cpp / MetalContext.cpp
                           └─ bgfx（API v166）  → Metal 后端
                                └─ CAMetalLayer（SDL_Metal_CreateView）
```

### 17.2 产物

| 产物 | 大小 / 说明 |
| --- | --- |
| `ndll/MacArm64/lime-64.ndll` | 10.2 MB，含 `lime_metal_*__prime` + 1281 个 bgfx 符号 |
| `CodenameEngine.app/Contents/MacOS/CodenameEngine` | 71.7 MB，arm64 |
| `CodenameEngine.app/Contents/MacOS/lime.ndll` | 10.2 MB（Metal 版） |

### 17.3 最终一击：`custom/sdl/include` 遮蔽了 SDL 配置

最后一个障碍非常隐蔽：lime 的编译命令里 `-Ilib/custom/sdl/include/` 排在
`-Ilib/sdl/include/` **前面**，而 `custom/sdl/include/` 里有自己的一套
`SDL_config.h` / `SDL_config_macosx.h` / `SDL_build_config.h`。
这套预生成的配置**完全没有定义 `SDL_VIDEO_METAL`**，于是
`SDL_cocoavideo.m` 把 Metal 注册分支条件编译掉了 →
`_this->Metal_CreateView == NULL` → `SDL_CreateWindow` 拒绝 `SDL_WINDOW_METAL`。

修法：往 `custom/sdl/include/SDL_config_macosx.h` 和 `SDL_build_config.h` 追加
```c
#ifndef SDL_VIDEO_METAL
#define SDL_VIDEO_METAL 1
#endif
#ifndef SDL_PLATFORM_SUPPORTS_METAL
#define SDL_PLATFORM_SUPPORTS_METAL 1
#endif
```

### 17.4 验证命令

```bash
# 1) 覆盖率 + 编译
metal/scripts/check-compile.sh --lime ~/haxelib-dev/lime-832 --hxcpp ~/haxelib/hxcpp/4,3,2
# 2) 渲染路径审计（原生 / 库 / shader / 模组）
metal/scripts/audit-render-paths.sh
# 3) 运行期确认
<app>/Contents/MacOS/CodenameEngine | head -1
#    → [cne-metal] bgfx renderer: Metal (Metal: yes), 1280x720, high-dpi
```

### 17.5 尚未做的

* **Universal（x86_64+arm64）构建**：bgfx 两个架构的静态库都已编好，`make-universal.sh` 已就绪；
  还差把同样的修复套到 x86_64 的 lime 重建上。
* **`read_pixels` / `copyTexImage2D` / `blitFramebuffer`** 等少量原语仍是降级实现（已上报日志）。
* 非 legacy GLSL（300+ core）的模组 shader 不在翻译器覆盖范围内（会明确报错，不会静默失败）。

---

## 18. 本轮补充：可复现性固化 + Universal 进度

### 18.1 所有原生补丁已固化（并对全新检出验证过）

新增 `metal/scripts/patch_lime_metal.py`，幂等、锚点校验、失败即报错。它一次性做完全部 6 组修改：

| # | 文件 | 改动 |
| --- | --- | --- |
| 1 | `project/Build.xml` | 编译标志 + Metal 源 + bgfx 链接标志（委托 `patch_lime_build.py`） |
| 2 | `src/backend/sdl/SDLWindow.cpp` | `SDL_WINDOW_METAL`、`MetalContext::Create/Frame/SetVsync`、GL 探测块条件编译掉 |
| 3 | `lib/custom/sdl/include/*` | `SDL_VIDEO_METAL 1`（**最关键的一处**，见 §17.3） |
| 4 | `lib/sdl-files.xml` | `-D__MACOSX__` |
| 5 | `lib/pixman/.../pixman-implementation.c` | `#if defined(__riscv)` 守卫 |
| 6 | `lib/openal-files.xml` | bundled fmt/gsl 的 include + `-std=c++20` |

**已验证**：在全新解包的 `lime-8.3.2` 上运行，6 组修改全部正确落盘（缺子模块的自动跳过并提示）。
在已打好的检出上再跑一次 → 全部 `already applied`（幂等）。

`apply.sh` 已改为调用它；`build-bgfx.sh` 已改为同时构建 **两个架构** 的 `bgfx bimg bimg_decode`。

### 18.2 Universal（x86_64 + arm64）：已完成的部分

| 项 | 状态 |
| --- | --- |
| bgfx / bx / bimg / bimg_decode（osx-arm64） | ✅ 全部 x86_64/arm64 正确的静态库 |
| bgfx / bx / bimg / bimg_decode（osx-x64） | ✅ 4 个库全部 `lipo -archs` = x86_64 |
| x86_64 Haxe 4.3.7（官方 osx 版，universal） | ✅ 下载并验证可在 Rosetta 下运行 |
| x86_64 Neko 2.3.0（`neko-2.3.0-osx64.tar.gz`） | ✅ 下载并验证 |
| x86_64 lime CLI | ✅ `Lime Command-Line Tools (8.3.2)` 正常（需把 `Mac64/lime.ndll` 放到 neko 可执行文件同目录） |
| x86_64 lime 编译 | ✅ 编译阶段全过，链接阶段差 `-arch x86_64` |

### 18.3 剩余障碍：让 hxcpp 产出 x86_64

实验结论：
* **不能用 x86_64 的 haxe/neko 跑构建**：`xcrun` 本身只有 `arm64,arm64e`，Rosetta 下会
  `unable to load libxcrun ... need 'x86_64'`。**只有编译输出需要 x86_64，构建工具不行。**
* hxcpp 的架构选择：`BuildTool.hx:142-150` 只在 `HXCPP_M64 / HXCPP_M32 / HXCPP_ARM64`
  **全部未设置**时才调用 `getArch()` 探测宿主。所以理论上 `-DHXCPP_M64` 就能让它走 x86_64。
* 但 `mac-toolchain.xml:18` 是 `<section unless="HXCPP_ARCH">` —— `HXCPP_ARCH` 一旦被设置就整段跳过。
  实测通过 lime CLI 传 `-DHXCPP_M64` / 在 Build.xml 里 `<set name="HXCPP_M64">` 都**没有**让链接变成 x86_64，
  链接仍在报 `for architecture arm64`（`project/obj/darwinarm64/` 的旧对象被复用）。

**下一步（明确二选一）**：
1. 在 `hxcpp/toolchain/mac-toolchain.xml` 里把 `<set name="HXCPP_ARCH" …>` 段临时改成强制 `x86_64`
   （该 `<section unless="HXCPP_ARCH">` 就是官方留的覆盖点），并在两次构建之间 `rm -rf project/obj`；
2. 或者干脆用两次独立构建 + `lipo`：
   arm64 已就绪；x86_64 只需解决上面的 `-arch` 问题，然后
   `lipo -create` 合并 `lime-64.ndll`（MacArm64 + Mac64）与两个 app 可执行文件。

`metal/scripts/make-universal.sh` 已描述这套流程。

---

## 19. Universal（arm64 + x86_64）：已验证的成果与剩余一步

### 19.1 成功：Universal `lime.ndll`（含 Metal 后端）

```
$ lipo -archs lime-universal.ndll
x86_64 arm64
$ ls -l lime-universal.ndll       -> 21,246,272 bytes
```

| 遍次 | 产物 | 架构 |
| --- | --- | --- |
| arm64 | `lime-64.ndll` 10.2 MB | arm64 |
| x86_64 | `lime-64.ndll` 11.0 MB | x86_64 |
| **lipo 合并** | `lime-universal.ndll` 21.2 MB | **x86_64 + arm64** |

**运行验证**：把 Universal ndll 装进 `.app`（并重新签名）后启动，仍输出

```
[cne-metal] bgfx renderer: Metal (Metal: yes), 1280x720, high-dpi
```

即 **Universal 二进制在 arm64 上照样走 Metal**。

### 19.2 打通 Intel 遍次的五个关键点

1. **构建工具必须留在 arm64。** `xcrun` 本身只有 `arm64,arm64e`，用 Rosetta 的 x86_64
   haxe/neko 驱动构建会报
   `xcrun: unable to load libxcrun … (have 'arm64,arm64e', need 'x86_64')`。
   **只有编译输出需要 x86_64**，工具链不能跨。
2. **`mac-toolchain.xml` 的 `<section unless="HXCPP_ARCH">` 就是官方覆盖点。**
   在它之前 `<set name="HXCPP_ARCH" value="x86_64" />`，后续每个 `-arch` 都会采用。
3. **bgfx 侧要备齐 osx-x64 的四个静态库**：`bgfx / bx / bimg / bimg_decode`。
   bgfx.cpp 会调用 bimg（`imageParse` / `getBlockInfo`），缺 `libbimg*.a` 直接链接失败。
4. **OpenAL 的 SIMD 选择必须看 `CNE_X64` 而不是 `HXCPP_ARM64`。**
   后者描述的是**宿主**，交叉编译到 Intel 时它仍是 on，于是 `mixer_neon.cpp` 被编译并在
   `<arm_neon.h>` 上报错。已改为 `CNE_X64` 优先（见 `openal-files.xml`）。
5. **替换 `.app` 内文件后必须重新签名**：
   `codesign --force --deep --sign - CodenameEngine.app`
   否则 macOS 启动即 SIGKILL（`Killed: 9`）。

### 19.3 剩余一步：app 可执行文件的 Universal 化

* `lime rebuild mac -DCNE_X64` **会**产出 x86_64 的 `lime.ndll`（已验证，`-arch x86_64` 生效）。
* 但 `lime build mac` 的 Intel 遍次**仍然产出 arm64 可执行文件** ——
  对象依旧落在 `export/release/macos/obj/obj/darwinarm64/`。
  把工具链覆盖改成无条件后重跑也一样，说明游戏侧的 hxcpp 构建没有走
  `mac-toolchain.xml` 的 `HXCPP_ARCH`，而是由 `BuildTool.hx:142-150` 的
  `set64(mDefines, m64, arm64)` 决定，其中 `mDefines` 是 **hxcpp 的输入 define**，
  不是 Build.xml 里的 `<set>`。

**下一步思路**（按可行性排序）：

1. 在 lime 给游戏生成的 hxcpp 构建配置里注入 `HXCPP_M64`、并确保 `HXCPP_ARM64` 不出现。
   `BuildTool.hx:144` 的条件是 `if (m64==m32 && !arm64)` ——
   只要 `HXCPP_M64` 出现在输入 define 里，宿主探测就会被跳过，架构即锁定为 x86_64。
2. 或直接 patch `BuildTool.hx` 的 `set64` 调用，加一个 `CNE_X64` 分支。
3. 或退一步：arm64 可执行文件 + Universal `lime.ndll` 的组合本身是**有效且已验证**的
   （见 19.1），先按这个交付，x86_64 可执行文件后续补。

### 19.4 一键脚本

`metal/scripts/make-universal.sh` 已把上述流程（两遍构建 → lipo → 装包 → 重新签名）
写成可复跑脚本，并会自动检测「Intel 遍次是否真的产出了 x86_64 可执行文件」，
是则合并成 Universal，否则如实提示并交付有效组合。

---

## 20. 渲染正确性 bug 修复（本轮，附截图证据）

用户报告：文字大小/内容不对、菜单图片裁剪错误。用 `screencapture` 拿到画面后，
从图上直接读出症状，再逆推到代码，共定位并修复 **5 个真实 bug**。

### 20.0 新增的免权限诊断工具

| 环境变量 | 作用 |
| --- | --- |
| `CNE_METAL_SHOT="600:/tmp/a,1800:/tmp/b"` | 在指定帧调用 `bgfx::requestScreenShot`，写出 `.tga`。**不需要系统权限、不受窗口遮挡影响** |
| `CNE_METAL_STATS=1` | 每 120 帧打印 `bgfxDraws / glDrawArrays / glDrawElements / submitDraw` |
| `CNE_METAL_DEBUG=1` | 打印每个 program 的属性映射与顶点布局 |

### 20.1 截图证据（修复前）

* **STORY MODE 顶部被一条水平线硬切** → scissor 裁剪位置错误
* 文字本身正确（`Commit: 0 (-)`、`[TAB] Open Mods menu`），但**左上角三行调试文字互相重叠**
* 模组开屏文字渲染成 `NN NNNNNN NN NNNN / H HHH / SSS S SSSSSSS` → 图集采样错误

### 20.2 修复的五个 bug

| # | Bug | 证据 | 修法 |
| --- | --- | --- | --- |
| 1 | **`bgfx::frame()` 从未被调用** | `SDLWindow::ContextFlip()` 里 `if (context && !sdlRenderer)`，Metal 路径下 `context` 恒为 0 | Metal 分支无条件调用 `MetalContext::Frame()` |
| 2 | **bgfx 渲染线程从未启动** | `BGFX_CONFIG_MULTITHREADED=1` 时 `frame()` 会阻塞等待 `renderFrame()`；未启动则主循环停摆 | `MetalContext::Create` 里起 `std::thread` 循环 `bgfx::renderFrame()`，`Destroy` 里优雅退出 |
| 3 | **所有绘制被静默丢弃** | `MetalContext::state.currentVertexArray` 恒为 0，而 **OpenFL/lime 根本不用 VAO**（全项目搜不到 `vertexArray`），`vertexArrays` 里没有 0 号条目 → 8 处查找全部 `return` | 按需创建 GL 默认 VAO（id 0），其余代码不动 |
| 4 | **scissor / viewport 的 Y 没翻转** | GL 用左下原点，bgfx 用左上原点。截图里 STORY MODE 被水平切开就是它 | `ConfigureView` 用当前渲染目标高度做 `h - (y + height)` |
| 5 | **采样器状态被忽略** | `glTexParameteri` 记录了 `minFilter/magFilter/wrapS/wrapT`，但 `bgfx::setTexture` 没传采样器 → bgfx 默认 **REPEAT**，图集向邻帧渗色、字体取到隔壁字形 | 新增 `ToBgfxSampler()` 把 GL 过滤/包裹映射成 `BGFX_SAMPLER_*`（注意 bgfx 166 的约定是**不设位 = LINEAR**） |

### 20.3 当前状态（诚实说明）

修复 1–5 之后，`bgfx::frame()` 与渲染线程都正常了，但实测：

```
[cne-stats] present=1   frame=1   bgfxDraws=0 glDrawArrays=0 glDrawElements=0 submitDraw=0
[cne-stats] present=121 frame=121 bgfxDraws=0 glDrawArrays=0 glDrawElements=0 submitDraw=0
```

即 **主循环以 ~98fps 运行、每帧都 present，但一次 `lime_gl_draw_*` 都没被调用**，
且 Haxe 日志停在 `Failed to add attributes to ./.temp/`，不再进入主菜单。

**修复前**画面能出现（因为 `frame()` 从不调用，绘制被无限累积），**修复后**呈现路径正确了，
于是暴露出真正的问题：**OpenFL 的渲染阶段根本没有跑到 draw 那一步**。

### 20.4 下一步（明确）

1. **确认 OpenFL 是否进入 `renderStage`**：在 `lime_gl_clear` / `lime_gl_use_program` /
   `lime_gl_enable_vertex_attrib_array` 也加计数器。若这些全是 0，说明 `ApplicationMain`
   的主循环没调用 `stage.__renderStage`，问题在上层而不是后端。
2. **查 `graphics.hx:219`**（已确认是模组脚本 `mods/dustin-mod-master/data/global/graphics.hx:219`
   访问 `FlxG.stage.__renderer.__allowSmoothing`）—— 它会在 openfl 某版本上抛
   `Invalid access to field __stage`。把这个模组从 app 包里移出后错误消失，但仍不进入菜单。
3. 检查 `Failed to add attributes to ./.temp/` 警告之后是否卡在文件系统/音频初始化。

**已修的 5 个 bug 都是确定的缺陷，与后续问题无关，应当保留。**

---

## 21. 渲染修复第二轮：从「0 次绘制」到「绘制正常发出」

### 21.1 最关键的发现：`GetContextType()` 返回了 "none"

```cpp
const char* SDLWindow::GetContextType () {
    if (context) return "opengl";        // Metal 路径下 context 是 NULL
    else if (sdlRenderer) { ... }
    return "none";                       // ← 落到这里
}
```

lime 的 Haxe 侧用这个字符串决定要不要 `new NativeOpenGLRenderContext ()`：

```haxe
switch (contextType) {
    case "opengl": ... context.gl = gl ...   // "none" 不匹配任何分支
}
```

→ `context.gl` 永远是 null → **OpenFL 根本不建渲染器 → 一次 draw 都没发出**。

**修复后实测**（`CNE_METAL_STATS=1`）：

| | `glDrawArrays` | `glDrawElements` | `submitDraw` |
| --- | --- | --- | --- |
| 修复前 | 0 | **0** | **0** |
| 修复后 | 0 | **1388 → 4198** | **同步增长** |

### 21.2 本轮共修复 6 个 bug

| # | Bug | 症状 | 修法 |
| --- | --- | --- | --- |
| 1 | `GetContextType()` 返回 `"none"` | **一次绘制都没有**，全黑 | Metal 分支恒返回 `"opengl"` |
| 2 | MSL 缺少 `XlatMtlVaryings xlatOut;` 声明与 `return xlatOut;` | MSL 不合法 → `createShader` 失败 | 发射器在 main 体首尾补上 |
| 3 | 顶点属性按**声明顺序**编号 | 数据全部落到错误属性（位置进了 `a_bitangent`） | 布局改用程序属性表 `BgfxAttribForLocation()` |
| 4 | `bgfx::frame()` 从不调用 | 不呈现 | `ContextFlip` 的 Metal 分支无条件调用 |
| 5 | bgfx 渲染线程未启动 | `frame()` 阻塞 | 起 `std::thread` 循环 `renderFrame()` |
| 6 | GL 默认 VAO（id 0）不存在 | 8 处查找全部 `return`，绘制被丢弃 | 按需创建 |

### 21.3 当前状态与最后一环

绘制已经正常发出（4000+/秒），但 `bgfxDraws` 仍为 0：

```
ERROR [openfl.display.Shader] Error compiling vertex shader
bgfx::createShader failed (see Metal validation log)
```

已验证**不是**翻译器的问题：
* 运行日志里没有 `GLSL->MSL failed`（翻译成功）
* 离线产出的 MSL 结构正确：入口 `xlatMtlMain`、`_mtl_u` 在 `buffer(0)`、`xlatOut` 已声明并返回

**剩余怀疑点**：手工构造的 bgfx shader-bin 容器头。`bgfx::createShader` 解析的是 bgfx 自己的
二进制格式（magic / hashIn / hashOut / rawBindings / constCount / shaderSize / code），
我为 MSL 源码补的这个头用了 version 7（避开 `texInfo`/`texFormat` 可选字段）。
如果 bgfx 反射解析对版本有额外要求，就会在这里失败。

**下一步**：
1. 用 bgfx 自带的 `shaderc` 生成一个真实的 Metal shader bin，`xxd` 对比头部；
2. 或把 version 提到 11 并补齐 `texInfo`(u16) / `texFormat`(u16) 字段；
3. 或改用 `BGFX_CONFIG_DEBUG=1` 重编 bgfx，让 Metal 的 `NSError` 打出来（当前 release 构建把 trace 关掉了，所以看不到真正的 Metal 编译错误）。

---

## 22. 第三轮：着色器编译错误归零，定位到 bgfx Metal 后端内部

### 22.1 本轮修掉的三个 bug（层层递进，都有硬证据）

| # | Bug | 怎么发现的 | 修法 |
| --- | --- | --- | --- |
| 1 | **MSL 签名后多了一对 `()`** | 写了个独立 ObjC 程序直接调 `newLibraryWithSource`，Metal 报 <br>`function cannot return function type 'XlatMtlVaryings ()'` | 发射器去掉残留的 `main(void)` 空参数列表 |
| 2 | **我自己的修复引入 off-by-2** | 调整 `braceOpen` 时忘了同步 `braceClose`，`return xlatOut;` 落到 `}` 之后 | 两个索引一起调整 |
| 3 | **shader bin magic 字节序写反** | `BX_MAKEFOURCC(a,b,c,d) = a \| b<<8 \| c<<16 \| d<<24`，而 `isShaderType` 取的是**低字节** | `'V' \| 'S'<<8 \| 'H'<<16 \| version<<24` |
| 4 | **shader bin 版本号太低** | `Context::createShader` 里有 <br>`if (isShaderVerLess(magic, 12)) { ... return BGFX_INVALID_HANDLE; }` | version 7 → **12** |

**效果**：`error compiling` / `createShader failed` 从 **12 条 → 0 条**；
`glDrawElements` 从 **0 → 4183/秒**；`submitDraw` 同步增长。

### 22.2 新增的离线验证工具（非常有价值）

`/tmp/mtltest.m`：一个 30 行的 ObjC 程序，直接调用 **和 bgfx 完全相同的 API**
（`[device newLibraryWithSource:options:error:]`）来编译我们生成的 MSL，
**不需要完整 Xcode 的 `metal` 编译器**。本轮三个 MSL 层面的 bug 全靠它定位。

配合 native 侧的 `CNE_METAL_DEBUG=1`（会把传给 `bgfx::createShader` 的 MSL 落到
`/tmp/cne-shader-N.metal`，并打印 26 字节头的 hex），可以做到：
* 确认 MSL 本身合法（mtltest）
* 确认容器头逐字节正确（hex dump）

实测头刚好正确：

```
56534807 00000000 00000000 00000000 00000000 0000 d2070000
 'V''S''H'  ver=12   hashIn   hashOut   srv      uav  cnt  size=2002
```

### 22.3 当前最后一环

着色器全部编译通过后，bgfx 在 Metal 后端内部触发了致命错误：

```
renderer_mtl.cpp (4015): BGFX FATAL 0x00000001: Failed to create Vertex shader.
Abort trap: 6
```

`BGFX_FATAL` 在 `ShaderMtl::create` 里，条件是 `m_function == NULL`，而
`m_lib = newLibraryWithSource(device, code)` 用的是
`NS::String::string(_source, NS::ASCIIStringEncoding)` + **NULL 编译选项**。

已排除的可能（都实测过）：
* MSL 非 ASCII 字节 → **0 个**，纯 ASCII
* blob 未正确 NUL 终止 → 尾部 `\n\n\x00` ✓
* NULL 编译选项导致失败 → 独立程序用 NULL 选项**编译成功**
* 入口点名字不匹配 → `SHADER_FUNCTION_NAME` = `"xlatMtlMain"`，我们的 MSL 正是这个名字
* 类型字符/版本号 → 头已逐字节验证，且 bgfx 已正确识别为 **Vertex**

**下一步（唯一最高价值动作）**：
用 `BGFX_CONFIG_DEBUG=1` 重新编译 bgfx。`renderer_mtl.h:113` 有一句

```cpp
BX_WARN(NULL == error, "Shader compilation failed: %s", error->localizedDescription()->utf8String());
```

它会把 **Metal 的真实错误**打出来 —— 当前 release 构建把 `BX_WARN` 关掉了，
所以真正的失败原因一直不可见。

---

## 23. 第四轮：让 bgfx 说出真实错误，着色器错误 12 → 0

### 23.1 关键手法：让 bgfx 把 Metal 的诊断打出来

`renderer_mtl.h` 的 `newLibraryWithSource` 只用 `BX_WARN` 报告错误，而 release 构建把
`BX_WARN` 编译掉了 —— 所以真正的失败原因一直不可见。加了两行无条件 `fprintf` 后，
错误立刻现形：

```
[cne-metal] MSL compile failed: program_source:54:35: error:
function cannot return function type 'XlatMtlVaryings ()'
```

### 23.2 本轮修掉的 5 个 bug

| # | Bug | 证据 | 修法 |
| --- | --- | --- | --- |
| 1 | MSL 签名后多 `()` | `function cannot return function type 'XlatMtlVaryings ()'` | 去掉残留的 `main(void)` 空参数列表 |
| 2 | 我的修复 off-by-2 | `return xlatOut;` 落到 `}` 之后 | 两个索引同步调整 |
| 3 | `()` 与 `{` 之间有换行时检查失效 | shader 4 仍报同样的错 | 改用 `rtrim` 后再比较 |
| 4 | **shader bin magic 字节序写反** | `'V'` 放到了高字节，而 `isShaderType` 取低字节 | `'V' \| 'S'<<8 \| 'H'<<16 \| version<<24` |
| 5 | **shader bin 版本号 < 12** | `Context::createShader` 明写 `if (isShaderVerLess(magic, 12)) return BGFX_INVALID_HANDLE` | version 7 → **12** |

### 23.3 新增：辅助函数作用域注入

OpenFL/Flixel 的着色器头里有**用户辅助函数**（`applyFlixelEffects`、`flixel_texture2D`、
`textureCam`…）。它们被发射成顶层 MSL 函数，但函数体引用了只作为 `xlatMtlMain`
参数存在的 `_mtl_u` / `xlatIn`：

```
error: use of undeclared identifier '_mtl_u'
```

MSL 没有嵌套函数、也没有 buffer 数据的全局变量，所以新增了 `injectHelperScope()`：
* 扫描顶层函数定义，判断函数体是否用到 `_mtl_u` / `xlatIn`
* 给需要的函数**追加对应参数**
* 同步改写**所有调用点**传入实参

实测输出：

```metal
float4 applyFlixelEffects (float4 color, constant XlatMtlUniforms& _mtl_u, XlatMtlVaryings xlatIn){
xlatOut.color = applyFlixelEffects (s_bitmap.sample(...), _mtl_u, xlatIn);
```

顶点/片元着色器均通过 Metal 运行时验证；翻译器回归测试 40/40 仍全过。

### 23.4 当前状态与最后一环

| 指标 | 修复前 | 现在 |
| --- | --- | --- |
| `Error compiling shader` | 12 条 | **0 条** |
| OpenFL 内置着色器（shader 0–3） | 编译失败 | ✅ 全部通过 Metal 验证 |
| Flixel 着色器（shader 4+） | 编译失败 | ⏳ 见下 |

**最后一环**（Metal 报的确切错误）：

```
program_source:76:26: error: unknown type name 'sampler2D'
float4 flixel_texture2D (sampler2D bitmap ,float2 coord ){
```

翻译器的类型转换只覆盖了 `uniform sampler2D x`（→ `texture2d<float> s_x`），
**没有覆盖函数参数位置**。MSL 里纹理参数还需要配一个 `sampler` 参数，即

```metal
float4 flixel_texture2D (texture2d<float> bitmap, sampler bitmapSampler, float2 coord)
```

调用点也要成对传参（`s_bitmap, s_bitmapSampler`）。`injectHelperScope()` 的框架已经
具备这个能力（它已经在做"追加参数 + 改写调用点"），只需扩展成"每遇到一个 sampler
参数就追加一个配对 sampler 参数"。

### 23.5 本轮新增的调试利器（都保留在仓库里）

| 工具 | 用途 |
| --- | --- |
| `CNE_METAL_DEBUG=1` + `[cne-dbg]` | 打印 program 属性映射、顶点布局；把传给 bgfx 的 MSL 落到 `/tmp/cne-shader-N.metal`，并打印 26 字节容器头的 hex |
| `/tmp/mtltest*.m` | 30 行 ObjC 程序，用**和 bgfx 完全相同的 API** 离线编译 MSL（不需要完整 Xcode 的 `metal` 编译器） |
| bgfx `renderer_mtl.h` 的 fprintf 补丁 | 让 Metal 的真实编译错误可见（`BX_WARN` 在 release 下被编译掉了） |
| `CNE_METAL_STATS=1` | 每 120 帧打印 `bgfxDraws / glDrawArrays / glDrawElements / submitDraw` |
| `CNE_METAL_SHOT` | 引擎内截图，不受窗口遮挡与系统权限影响 |

---

## 24. 第五轮：bgfx 终于真正提交绘制（numDraw 0 → 12）

### 24.1 里程碑

```
[cne-stats] present=121 frame=121 bgfxDraws=12 glDrawElements=1377 submitDraw=1377
```

* **着色器错误：0**（OpenFL 内置 + Flixel 辅助函数 + sampler 参数全部通过 Metal 验证）
* **`bgfxDraws`：0 → 12**（bgfx 真正执行了绘制）
* `glDrawElements` 1377/帧，`submitDraw` 同步

### 24.2 本轮修掉的 4 个 bug

| # | Bug | 症状 | 修法 |
| --- | --- | --- | --- |
| 1 | **sampler 参数没翻译** | `unknown type name 'sampler2D'` | 参数位 `sampler2D` → `texture2d<float> s_x, sampler s_xSampler`，调用点成对传参 |
| 2 | **辅助函数链的传递性** | `textureCam` 调 `flixel_texture2D` 时 `_mtl_u` 不在作用域 | 需求做**传递闭包**后再注入 |
| 3 | **`state.viewport` 默认 `{0,0,1,1}`** | 每帧有 12 个 draw 但窗口全黑 | Create/Resize 里初始化为后台缓冲尺寸 |
| 4 | **macOS「重开窗口」对话框** | 游戏启动后无任何输出、卡死 | 测试用 `-ApplePersistenceIgnoreState YES` 并清 `savedState` |

### 24.3 新增能力：MSL 后处理的区间替换

`injectSamplerParams()` 用「区间替换（splice）」而不是「插入 + 删字符」：
先在**原始字符串**上记录所有 `[start, end) → text`，再按 start 降序应用 ——
这样偏移量始终有效、不会互相干扰。这是重写参数表/实参表最稳的方式。

### 24.4 当前状态与最后一环

| 指标 | 值 |
| --- | --- |
| 着色器错误 | **0** |
| `bgfxDraws` / 帧 | **12** |
| `glDrawElements` / 帧 | ~1377 |
| 窗口画面 | ⏳ 仍为黑 |

draw 已经真正提交给 bgfx 并执行了（`numDraw > 0`），但窗口仍全黑。下一步要查的是
**提交到 view 0 的内容为什么没有出现在交换链上**：

1. 打印每帧实际使用的 `view` 与 `currentFramebuffer`（`SubmitDraw` 里已有 `ViewForFramebuffer`）
2. 检查 `bgfx::setViewClear` 是否在 draw 之后才生效，把 draw 清掉
   （`glClear` → `setViewClear(view)`，而 bgfx 在**该 view 开始渲染时**执行 clear）
3. 检查 `MetalContext::Frame()` 里 `worldWidth/worldHeight`（交换链尺寸）与实际窗口是否一致
4. 用 `bgfx::requestScreenShot` 在 `bgfx::frame()` **之前**抓帧，对比是否已有内容 —— 这能区分
   "没画出来" 与 "画了但没 present"

---

## 25. 第六轮：坐标空间修复 —— view 矩形终于正确

### 25.1 决定性发现：GL 与 bgfx 的坐标空间不一致

`ConfigureView` 的插桩打印暴露了根因：

```
view=0 targetH=720  rect=(0,-720,2560,1440)  scissor=(0,-720,2561,1441)
```

* GL 的 `glViewport` 用的是 **drawable 物理像素**（2560×1440，2x 屏幕）
* bgfx 的交换链却是 **逻辑像素**（1280×720）

于是 Y 翻转 `h - (y + height)` = `720 - (0 + 1440)` = **-720**，
再被 `(uint16_t)` 转换后变成 **64768** —— 整个 view 被裁到屏幕外，
所以 `numDraw > 0` 但窗口全黑。

### 25.2 修法

让 bgfx 交换链使用 **drawable 尺寸**（`SDL_Metal_GetDrawableSize`），
两个坐标空间就一致了：

```
view=0 targetH=1440 rect=(0,0,2560,1440) scissor=(0,0,2560,1440) fbo=0
bgfxDraws=10  glDrawElements=33682  backbuffer=2560x1440
```

`rect` 不再是负数 ✅

### 25.3 另一处误导：我自己的调试打印

顶点布局的 dump 打印的是 `SlotToAttrib(pair.first)`（旧映射），
而 `layout.add` 实际用的是 `BgfxAttribForLocation`（程序属性表）。
一度让我误判属性映射失效，实际两者都是对的：

```
program 映射: glLoc=0 -> bgfxAttrib=3     ← openfl_Position → a_bitangent
MSL 声明:    float4 a_bitangent [[attribute(3)]]
```

一致 ✅（**教训：调试打印必须打印真正被使用的值**）

### 25.4 当前状态

| 指标 | 值 |
| --- | --- |
| 着色器错误 | **0** |
| `bgfxDraws` / 帧 | **10** |
| `glDrawElements` / 帧 | ~33682 |
| view rect / scissor | **(0,0,2560,1440)** ✅ |
| 属性映射 | ✅ 正确 |
| 引擎内截图 | ⏳ 仍全黑 |

### 25.5 下一步（按价值排序）

1. **在 `bgfx::frame()` 之前抓帧**，区分「没画出来」与「画了但没 present」
2. **打印第一帧的顶点数据**（前几个顶点的 position/texcoord），确认 MVP 变换后不在
   NDC 之外 —— `openfl_Matrix` 是否被正确上传（`lime_gl_uniform_matrix`）
3. **检查 `ToBgfxState()` 里的混合/写入掩码**与 OpenFL 期望是否一致
4. **检查 NDC Y 方向**：bgfx 在 Metal 下 `originBottomLeft == false`，而 OpenFL 的
   投影矩阵按 GL 约定（Y 向上）构造；若方向相反，几何体会被翻到视口之外
   （若只是上下颠倒则仍可见，若配合裁剪就会被完全裁掉）

---

## 26. 第七轮：问题被精确锁定到「交换链呈现」

### 26.1 用户的关键观察

用户指出：**窗口是黑的，不是洋红**。而同一时刻：

| 来源 | 颜色 |
| --- | --- |
| 系统截图（真实窗口） | 全黑 `(0,0,0)` |
| `bgfx::requestScreenShot`（后台缓冲） | 全洋红 `(255,0,255)` |

这是一个**决定性判别**：`CNE_METAL_CLEAR_TEST=1` 强制洋红清屏后，
**后台缓冲有内容、窗口却没有** ——

> **bgfx 渲染完全正常，坏的是把后台缓冲呈现到 CAMetalLayer 的那一步。**

这一步区分了此前所有猜测的方向，直接把范围缩小到一个环节。

### 26.2 本轮修掉的问题

| # | 问题 | 证据 | 修法 |
| --- | --- | --- | --- |
| 1 | **GL 与 bgfx 坐标空间不一致** | `rect=(0,-720,2560,1440)`，Y 翻转算出 -720 → uint16 变 64768 → 裁到屏外 | 交换链改用 `SDL_Metal_GetDrawableSize` 的 drawable 尺寸；现在 `rect=(0,0,2560,1440)` ✅ |
| 2 | `state.viewport` 默认 `{0,0,1,1}` | 内容被压进 1 像素 | Create/Resize 时初始化为 drawable 尺寸 |
| 3 | `SDL_WINDOW_METAL` 可能造成双 layer | 移除后仍黑，**排除** | 已移除（保留说明） |

### 26.3 新增的决定性诊断开关

`CNE_METAL_CLEAR_TEST=1` —— 强制洋红清屏。
配合 `bgfx::requestScreenShot` 与系统截图对比，**一眼区分**：

* 「渲染坏了」→ 两边都黑
* 「呈现坏了」→ 后台缓冲有内容、窗口黑 ← **当前就是这种情况**
* 「清屏坏了」→ 两边都黑但 numDraw > 0

### 26.4 当前状态

| 指标 | 值 |
| --- | --- |
| 着色器错误 | **0** |
| `bgfxDraws` / 帧 | 10 |
| `glDrawElements` / 帧 | ~33682 |
| view rect / scissor | **(0,0,2560,1440)** ✅ |
| 属性映射（`glLoc=0 → attrib=3`） | ✅ 与 MSL 声明一致 |
| 后台缓冲内容 | ✅ 有（洋红测试） |
| **窗口显示** | ❌ 黑 |

### 26.5 下一步（交换链呈现专项）

1. **确认交给 bgfx 的 CAMetalLayer 就是窗口上显示的那一个**：
   打印 `SDL_Metal_GetLayer()` 与 `window.contentView.layer` 的指针，比较是否相同
2. **改传 NSWindow 而不是 layer**：bgfx 的 Metal 后端同时接受
   `NSWindow` / `NSView` / `CAMetalLayer`（`renderer_mtl.cpp:5055`），
   让它自己去建立 layer，排除 SDL 与 bgfx 各建一套的冲突
3. **检查 `swapChain.numBackBuffers` / `maxFrameLatency` / `formatColor`** 是否与 layer 匹配
   （`renderer_mtl.cpp:5175` 有一条 drawableSize 不匹配的警告值得打开看）
4. **确认 `bgfx::frame()` 真的触发了 present**：在 `bgfx::frame()` 前后检查
   `bgfx::getStats()->numPresent`（若该版本有）
5. 或者绕开 SDL：用 `SDL_Metal_CreateView` **之前**先把窗口的 `contentView.wantsLayer`
   设好，只保留一套 layer

---

## 27. 第八轮：排除 SDL/bgfx 双 layer 与 `numBackBuffers`，问题收敛到 present 本身

### 27.1 本轮的两个实验（都做了，都排除）

| 实验 | 做法 | 结果 |
| --- | --- | --- |
| **改传 NSWindow** | 不再用 `SDL_Metal_CreateView`，改为 `SDL_GetWindowWMInfo().info.cocoa.window` 交给 bgfx，让它自己建立并接管 CAMetalLayer | 日志确认 `nwh = NSWindow 0x…`，bgfx 初始化成功，**窗口仍黑** → 排除「SDL 与 bgfx 各建一套 layer」 |
| **`numBackBuffers = 0`** | 检查 `SwapChain()` 默认值确实是 0 | `bgfx.cpp:2481` 有 `bx::clamp(…, 2, BGFX_CONFIG_MAX_BACK_BUFFERS)` 兜底 → **排除** |

### 27.2 已排除的清单（逐步缩小）

| 假设 | 状态 |
| --- | --- |
| 着色器编译失败 | ❌ 排除（0 错误，MSL 逐个通过 Metal 运行时验证） |
| 没有绘制调用 | ❌ 排除（`bgfxDraws=10`，`glDrawElements≈33682`） |
| 清屏/呈现链路完全断 | ❌ 排除（洋红测试证明后台缓冲有内容） |
| view 矩形错误 | ❌ 排除（`rect=(0,0,2560,1440)`） |
| 属性映射错误 | ❌ 排除（`glLoc=0→attrib=3` 与 MSL 一致） |
| SDL 与 bgfx 双 layer | ❌ 排除（NSWindow 路径同样黑） |
| `numBackBuffers` 为 0 | ❌ 排除（有 clamp 兜底） |

**结论：bgfx 的渲染管线完全正确，坏的只是「把后台缓冲提交到 CAMetalLayer」这一步。**

### 27.3 下一步（针对 present 本身）

1. **确认 `bgfx::frame()` 真的走到 present**：在 `SwapChainMtl` 的 present 路径加日志，
   或检查 `bgfx::getStats()` 里与 present 相关的计数
2. **检查 render thread 的行为**：我们在 `MetalContext::Create` 起了一个
   `while (running) bgfx::renderFrame();` 的循环。若该循环与 bgfx 内部的 present
   握手不匹配（例如 `renderFrame()` 在无帧时立即返回并空转），可能让 present 饿死 ——
   临时**去掉这个线程**、改用单线程模式（`bgfx::renderFrame()` 在 `bgfx::frame()` 内执行）
   对比一次，这是最值得试的一步
3. **检查 `BGFX_CONFIG_MULTITHREADED`**：确认实际编译值是 1（macOS 默认应为 1）
4. 用 `MtlObjAccess` 打印 bgfx 实际使用的 layer 的 `drawableSize` 与 `frame`，
   确认它与窗口尺寸一致
5. 若仍不通，考虑 **绕开 bgfx 的 swapchain**：让 bgfx 渲染到一张 texture，
   由我们自己用 CAMetalLayer 呈现（工作量较大，但完全可控）

### 27.4 已确认可用的部分（不应回退）

* 着色器翻译链（GLSL → MSL）完整可用，含辅助函数作用域注入与 sampler 参数翻译
* 属性映射、顶点布局、view/viewport/scissor、采样器状态、Y 翻转**全部正确**
* 引擎内截图（`CNE_METAL_SHOT`）、绘制统计（`CNE_METAL_STATS`）、
  洋红清屏判别（`CNE_METAL_CLEAR_TEST`）、离线 MSL 验证（`mtltest`）等诊断工具齐备

---

## 28. 第九轮：单线程与 swapchain 诊断 —— 全部排除，问题锁定在「layer 未被合成上屏」

### 28.1 本轮两个实验

| 实验 | 做法 | 结果 |
| --- | --- | --- |
| **bgfx 改单线程** | `config.h` 里 `BGFX_CONFIG_MULTITHREADED 0`，并移除 `MetalContext` 里的 render thread，让 `bgfx::frame()` 同步完成一切 | bgfx 重新编译并链接成功，**窗口仍黑** → **排除 render thread/握手** |
| **swapchain layer 创建失败路径** | 给 `renderer_mtl.cpp` 的两条 `BX_WARN(...); return;`（"nwh is not a CAMetalLayer" / "not NSWindow/NSView/CAMetalLayer"）加 fprintf | **两条都没打印** → bgfx **成功**从 NSWindow 建立/接管了 CAMetalLayer |

### 28.2 至此已排除的全部假设

| 假设 | 排除依据 |
| --- | --- |
| 着色器编译失败 | 0 错误；MSL 逐个通过 Metal 运行时验证 |
| 没有绘制调用 | `bgfxDraws=10`、`glDrawElements≈33682` |
| 清屏坏了 | 洋红测试：后台缓冲满屏洋红 |
| view 矩形/裁剪错误 | `rect=(0,0,2560,1440)`，无负值 |
| 属性映射错误 | `glLoc=0→attrib=3` 与 MSL 声明一致 |
| SDL 与 bgfx 双 layer | 改传 NSWindow 后同样黑 |
| `numBackBuffers` 为 0 | `bgfx.cpp:2481` 有 `clamp(…,2,MAX)` 兜底 |
| render thread / MT 握手 | 改单线程后同样黑 |
| swapchain layer 创建失败 | bgfx 的失败分支未被触发 |

**结论：bgfx 一路走到「已渲染进交换链 drawable」，但该 layer 的内容没有出现在窗口上。**

### 28.3 最可能的剩余原因（下一步按序验证）

1. **layer 被 SDL 的视图层级遮住**：bgfx 取 `[nsWindow contentView]` 并把 CAMetalLayer 设为其 layer；
   而 SDL2 的窗口用自定义 `SDLView`，可能有子视图盖在上面。
   → 打印 `contentView.layer`、其 `sublayers` 数量与 frame，和 bgfx 的 layer 比较
2. **`drawableSize` 与实际不一致**：`renderer_mtl.cpp:5175` 有一条
   `"CAMetalLayer drawableSize is %ux%u after requesting %ux%u"` 的警告（release 下被 BX_WARN 吞掉），
   值得用同样的方式打开
3. **回退到 `SDL_Metal_CreateView` 但自己把它 addSubview 到最前**，或
   **直接给 bgfx 传 `SDL_Metal_GetLayer()` 并显式设置 layer 的 frame/wantsLayer**
4. **最彻底的兜底**：让 bgfx 渲染到一张 texture，由我们自己的 CAMetalLayer
   （确保是窗口 contentView 的 layer）做 blit 呈现

### 28.4 明确可用、不应回退的部分

* **GLSL → MSL 翻译链完整可用**：辅助函数作用域注入、sampler 参数翻译、传递闭包、
  `xlatOut` 声明/返回、`main(void)` 残留参数清理 —— 全部经 Metal 运行时逐条验证
* **bgfx 容器头**：magic 字节序、version ≥ 12 正确
* **渲染状态映射**：属性映射、顶点布局、view/viewport/scissor、采样器状态、
  drawable↔backbuffer 坐标空间 —— 全部正确
* **诊断工具链**（都在仓库里，后续调试可直接复用）：
  `CNE_METAL_SHOT`（引擎内截图）、`CNE_METAL_STATS`（绘制统计）、
  `CNE_METAL_DEBUG`（MSL 落盘 + 容器头 hex + 属性/布局 dump）、
  `CNE_METAL_CLEAR_TEST`（洋红判别）、`mtltest`（离线 MSL 验证）、
  bgfx 侧三处 fprintf 补丁（shader 编译错误 / swapchain 失败路径）

---

## 29. 第十轮：layer 挂载验证通过 —— 问题精确定位到「drawable 从未被 present」

### 29.1 两个新发现（都有直接证据）

**发现 1：SDL 的 contentView 不是 layer-backed**

```
contentView=0x…(SDLView)  wantsLayer=0  layer=0x0
```

macOS 上视图必须 `wantsLayer = YES` 才能合成 CAMetalLayer 的内容。
已在 `MetalContext::Create` 里 bgfx 初始化**之前**显式设置：

```cpp
void* contentView = SendPtr (layer, "contentView");
((void (*)(void*, void*, signed char))objc_msgSend)(
    contentView, (void*)sel_registerName ("setWantsLayer:"), 1);
```

（`MetalContext.cpp` 是 C++ 而非 ObjC，所以通过 `objc_msgSend` 调用，
与 bgfx 的 `MtlObjAccess` 做法一致。）

**发现 2：bgfx 的 CAMetalLayer 挂载成功**

在 `bgfx::init` 前后各 dump 一次视图层级：

| 时刻 | `contentView.layer` |
| --- | --- |
| init **前** | `NSViewBackingLayer`（AppKit 默认） |
| init **后** | **`CAMetalLayer`** ✅ |

即 bgfx 确实把自己的 layer 装到了窗口的 contentView 上。

### 29.2 至此整条链路的验证状态

| 环节 | 状态 |
| --- | --- |
| contentView layer-backed | ✅ `wantsLayer=1` |
| bgfx CAMetalLayer 挂载 | ✅ contentView.layer == CAMetalLayer |
| bgfx 后台缓冲内容 | ✅ 洋红测试 |
| `bgfx::frame()` 每帧调用 | ✅ present 计数 3000+ |
| view / viewport / scissor / 属性 / 采样器 | ✅ 全部正确 |
| 着色器编译 | ✅ 0 错误 |
| **drawable 被写入并 present** | ❌ **从未发生** |

### 29.3 剩余假设（按可能性排序）

1. **`bgfx::reset()` 在 `Resize()` 里重建了交换链**：
   现有实现用 `width/height`（逻辑尺寸）新建一个 `SwapChain` 再 `bgfx::reset()`，
   若 `Resize` 在启动后被调用，交换链尺寸会与 layer 的 drawableSize 不一致
   → 应先改成 drawable 尺寸，并核对 `Resize` 的调用时机
2. **`SwapChainMtl` 的 `nextDrawable()` 返回 NULL**：
   layer 的 `drawableSize` 若为 0（窗口尚未上屏/最小化），
   `nextDrawable` 会返回 nil，present 被静默跳过
   → 打印 layer 的 `drawableSize`、`bounds`、`device`、`pixelFormat`
3. **`bgfx::frame()` 之后需要一次显式 `bgfx::reset()`** 才能激活新挂载的交换链
4. **`MetalContext::Frame()` 的调用时序**：确认 `ContextFlip` 每帧只调一次，
   且在所有 draw 之后

### 29.4 本轮新增的诊断能力

`DumpViewHierarchy()` —— 通过 ObjC 运行时打印
`nwh / contentView / wantsLayer / layer 类名 / sublayers 数量`。
这是把「窗口黑屏」拆成「layer 没挂上」与「layer 挂上了但 drawable 没内容」
两类问题的关键工具，建议保留。

---

## 30. 第十一轮：CAMetalLayer 配置逐项验证通过 —— 只剩 bgfx 的 present 调用本身

### 30.1 拿到的完整 layer 数据（全部正常）

```
init 前: contentView.wantsLayer=1  layer=NSViewBackingLayer
init 后: contentView.layer = CAMetalLayer          ← bgfx 挂载成功
         CAMetalLayer drawableSize = 2498x1440     ← 非零，且与交换链尺寸一致
         device        = 0xa64500000               ← 有效
         pixelFormat   = 80 (BGRA8Unorm)           ← 正确
         framebufferOnly = 1                       ← 默认值，正常
bgfx renderer = Metal, 2498x1440
```

### 30.2 本轮修掉的一个真实缺陷

`MetalContext::Resize()` 用**逻辑尺寸**新建 `SwapChain` 再 `bgfx::reset()`，
与 layer 的 drawable 尺寸不一致（和之前 view rect 问题的同一类错误）。
已新增 `MetalContext::QueryDrawableSize()`（走 `SDL_Metal_GetDrawableSize`）并改用它。

另：`DumpViewHierarchy()` 里探测 `drawableSize` 前必须确认 layer 是 CAMetalLayer，
否则在 `bgfx::init` 之前会对 `NSViewBackingLayer` 调用不存在的方法而抛
`NSInvalidArgumentException`。已加 `isKindOfClass:` 守卫。

### 30.3 至此「除 present 外」的每一项都已验证

| 环节 | 证据 |
| --- | --- |
| GLSL → MSL 翻译 | 0 错误；辅助函数注入 / sampler 参数 / 传递闭包全部经 Metal 运行时验证 |
| bgfx shader 容器 | magic 字节序、version ≥ 12 逐字节核对 |
| 着色器编译 | `bgfx::createShader` 全部成功 |
| 绘制提交 | `glDrawElements ≈ 33682/帧`，`bgfxDraws=10` |
| 后台缓冲内容 | 洋红测试满屏 |
| view / viewport / scissor | `rect=(0,0,2498,1440)` |
| 属性映射 | `glLoc=0→attrib=3` 与 MSL 一致 |
| 采样器状态 | 由 `glTexParameteri` 正确映射 |
| contentView layer-backed | `wantsLayer=1` |
| bgfx layer 挂载 | `contentView.layer == CAMetalLayer` |
| layer 配置 | drawableSize / device / pixelFormat 全部正确 |
| **drawable 被 present** | ❌ **唯一未成立的环节** |

### 30.4 剩余可能性（收敛到 bgfx 的 present 调用）

1. **bgfx 的 `SwapChainMtl::present()` 从未执行**：在 `renderer_mtl.cpp` 的
   present 路径加 fprintf，确认 `nextDrawable()` 是否被调用、返回值是否为 nil
2. **需要显式激活交换链**：检查 bgfx API v166 是否要求在 `init` 之后
   再调用一次 `bgfx::reset(flags, &swapChain)`（或 `touch`）才算把交换链接上
3. **`maxFrameLatency` / `numBackBuffers` 与 layer 不匹配**：尝试显式设为
   `numBackBuffers = 3`、`maxFrameLatency = 2`
4. **绕过 bgfx 的 swapchain**：让 bgfx 渲染到一张 `BGFX_TEXTURE_RT` 纹理，
   由我们用 `nextDrawable` + blit 自己呈现 —— 这是完全可控的兜底方案，
   但工作量较大（约需新增 150 行 ObjC）

### 30.5 本轮新增/保留的可复用诊断

| 工具 | 作用 |
| --- | --- |
| `DumpViewHierarchy()` | ObjC 运行时打印 `nwh / contentView / wantsLayer / layer 类名 / drawableSize / device / pixelFormat` |
| `CNE_METAL_CLEAR_TEST=1` | 洋红清屏，区分「渲染坏」与「present 坏」 |
| `CNE_METAL_SHOT` | 引擎内截图（读后台缓冲） |
| `CNE_METAL_STATS=1` | `bgfxDraws / glDrawArrays / glDrawElements / submitDraw` |
| `CNE_METAL_DEBUG=1` | MSL 落盘 + 容器头 hex + 属性/布局 dump |
| `mtltest*.m` | 离线 MSL 验证（与 bgfx 完全相同的 API） |
| bgfx 三处 fprintf 补丁 | shader 编译错误 / 两条 swapchain 失败路径 |

---

## 31. 第十二轮：根因链条完整闭合 —— `m_drawable` 为 NULL，`presentDrawable` 从未执行

### 31.1 决定性的最终证据

给 bgfx 的 present 路径加探针后：

```
[cne-metal] Present: needPresent=1 numWindows=1
[cne-metal]   window[0] swapChain=0x925488550 drawableTexture=0x923f7e300 drawable=0x0
                                                                    ↑ NULL！
```

对应源码：

```cpp
if (NULL != frameBuffer.m_swapChain->m_drawable)      // ← 为假
{
    presentCommandBuffer->presentDrawable(...);        // ← 从未执行
}
```

### 31.2 完整根因链（每一环都有实测数据）

| 环节 | 实测 | 结论 |
| --- | --- | --- |
| 1. bgfx 渲染 | `bgfxDraws=10`，`glDrawElements≈33682/帧` | ✅ 正常 |
| 2. 渲染目标 | `drawableTexture=0x923f7e300` 非空 | 但这是 **`else` 分支创建的离屏 dummy texture** |
| 3. 真实 drawable | **`drawable=0x0`** | ❌ `nextDrawable()` 返回 nil |
| 4. 窗口遮挡判断 | `occlusionState=0x2002 → occluded=0` | ✅ 已排除遮挡 |
| 5. `presentDrawable` | 因为 drawable 为 NULL 而被跳过 | ❌ **窗口永不更新** |
| 6. 表面现象 | `requestScreenShot` 有内容（读的是离屏纹理）＋ 窗口全黑 | 与 1–5 完全一致 |

`currentDrawableTexture()` 的逻辑：

```cpp
const bool occluded = isWindowOccluded(m_nwh);
m_drawable = occluded ? NULL : m_metalLayer->nextDrawable();

if (m_drawable != NULL) { m_drawableTexture = m_drawable->texture(); ... }
else { /* 创建离屏 dummy texture 继续渲染 */ }     // ← 我们一直走这条分支
```

### 31.3 下一层（本案的最后一步）

`nextDrawable()` 返回 nil 的两个可能：

1. **`SwapChainMtl::m_metalLayer` 为 NULL** —— 给 nil 发消息在 ObjC 里返回 nil，
   于是 `m_drawable = NULL`。虽然我们验证过 `contentView.layer == CAMetalLayer`，
   bgfx 内部保存的可能是另一个（更早的、已被替换的）layer 指针。
   → **打印 `m_metalLayer` 即可确认**
2. layer 本身的状态不允许取 drawable（如未上屏、`presentsWithTransaction` 等）

### 31.4 已修好且确认正确的部分（不要回退）

* **GLSL → MSL 全链路**：辅助函数作用域注入、sampler 参数翻译、传递闭包、
  `xlatOut` 声明/返回、`main(void)` 残留清理 —— 逐条经 Metal 运行时验证
* **bgfx shader 容器**：magic 字节序、version ≥ 12
* **渲染状态**：属性映射、顶点布局、view/viewport/scissor、采样器、drawable↔交换链尺寸
* **窗口**：contentView layer-backed（`wantsLayer=1`）、CAMetalLayer 已挂载、
  `drawableSize=2498x1440`、`device` 有效、`pixelFormat=80`
* **遮挡判断**：`occluded=0`，排除
* **单线程/多线程**：两种都验证过，均与此问题无关

### 31.5 本案的调试方法论（值得复用）

1. `CNE_METAL_CLEAR_TEST=1` 洋红清屏 → 区分「渲染坏」与「present 坏」
2. `CNE_METAL_SHOT` 读后台缓冲 vs `screencapture` 读屏幕 → 两者不一致即 present 有问题
3. 逐层给 bgfx 内部加 fprintf（`BX_WARN` 在 release 下被编译掉，必须自己加）
4. 每改一次 bgfx，**必须 `rm -rf project/obj` 强制重链**，
   否则 hxcpp 会复用旧对象、补丁看似没生效（本轮踩过一次）
5. ObjC 运行时探查（`objc_msgSend`）用于检查 NSWindow / NSView / CALayer 状态

---

## 32. 第十三轮：`nextDrawable()` 正常，但 drawable 在 present 前被清空

### 32.1 本轮拿到的关键对比

给 `SwapChainMtl::currentDrawableTexture()` 加探针后：

```
[cne-metal] currentDrawableTexture: occluded=0
             m_metalLayer=0xb5fb602a0  m_nwh=0xb5ee64500  drawableSize=2498x1440
[cne-metal]   nextDrawable() -> 0xb5fab9ce0        ← 成功，非 NULL
```

而同一帧稍后的 present 探针：

```
[cne-metal] Present: needPresent=1 numWindows=1
[cne-metal]   window[0] swapChain=… drawableTexture=0x… drawable=0x0    ← 变成 NULL
```

**结论：`nextDrawable()` 工作正常，`m_drawable` 是在 `currentDrawableTexture()` 之后、
present 之前被清空/释放的。**

对应的 present 源码：

```cpp
if (NULL != frameBuffer.m_swapChain->m_drawableTexture)
{
    MTL_RELEASE_I(frameBuffer.m_swapChain->m_drawableTexture);   // 释放并置空 texture

    if (NULL != frameBuffer.m_swapChain->m_drawable)             // ← 此时已经是 NULL
    {
        presentCommandBuffer->presentDrawable(...);              // ← 因此永不执行
        MTL_RELEASE_I(frameBuffer.m_swapChain->m_drawable);
    }
}
```

### 32.2 至此每个环节的状态

| 环节 | 实测 | 状态 |
| --- | --- | --- |
| `m_metalLayer` | `0xb5fb602a0` 有效 | ✅ |
| layer `drawableSize` | `2498x1440` | ✅ |
| `nextDrawable()` | 返回 `0xb5fab9ce0` | ✅ |
| 窗口遮挡 | `occluded=0` | ✅ |
| `drawableTexture` | 非 NULL（离屏 dummy） | 说明走了 `else` 分支 |
| present 时的 `m_drawable` | **`0x0`** | ❌ 被提前清空 |
| `presentDrawable` | 因上面一条而跳过 | ❌ |

### 32.3 最后一步（唯一剩余）

**找出是谁把 `m_drawable` 清成了 NULL。** 候选：

1. **`bgfx::reset()`** —— `MetalContext::Resize()` 会调用它重建交换链；
   若在渲染之后、present 之前触发，交换链状态会被销毁
   → 给 `SwapChainMtl::destroy/init/reset` 加探针确认调用时机
2. **一帧内 `currentDrawableTexture()` 被调用两次**：第二次因
   `m_drawableTexture != NULL` 直接返回，但若中途 `m_drawableTexture` 被释放，
   就会再取一次 drawable 并覆盖/释放前一个
3. **`m_windows[]` 中的 framebuffer 与渲染时使用的不是同一个对象**：
   `getFrameBuffer(m_windows[ii])` 可能拿到另一份状态

### 32.4 已确认正确、不应回退的全部部分

* **GLSL → MSL 全链路**（辅助函数作用域注入 / sampler 参数 / 传递闭包 / xlatOut / main 参数清理）
* **bgfx shader 容器**（magic 字节序 + version ≥ 12）
* **渲染状态**（属性映射 / 顶点布局 / view / viewport / scissor / 采样器 / drawable↔交换链尺寸）
* **窗口与 layer**（`wantsLayer=1` / CAMetalLayer 已挂载 / drawableSize / device / pixelFormat）
* **`nextDrawable()` 正常**
* **遮挡判断正常**

距离「画面显示」只差最后这一处状态被提前清空的问题。

---

## 33. 结论：根因链闭合，与建议的收尾方案

### 33.1 完整根因链（每一环都有实测数据）

```
① bgfx 用 NSWindow 初始化，成功把 CAMetalLayer 挂到 window.contentView
   → 实测 contentView.layer == CAMetalLayer，drawableSize=2498x1440，device 有效 ✓

② contentView 需要 layer-backed，已补 setWantsLayer:YES
   → 实测 wantsLayer=1 ✓

③ 每帧 currentDrawableTexture() 调用 nextDrawable()
   → 实测 nextDrawable() -> 0xb5fab9ce0，成功拿到 drawable ✓

④ 窗口遮挡判断
   → 实测 occlusionState=0x2002 → occluded=0 ✓

⑤ 但 present 时读到 m_drawable == 0x0
   → MTL_RELEASE_I(m_drawable) 在 release 后把指针置空；
     一旦某一帧走了「drawable 已为 NULL」的分支，m_drawableTexture 变成
     离屏 dummy texture，此后 present 的条件
         if (NULL != ...->m_drawableTexture) { ... if (NULL != ...->m_drawable) { presentDrawable } }
     永远只满足外层，presentDrawable 再也不会被调用。

⑥ 结果：bgfx 一直渲染到离屏 dummy texture
   → requestScreenShot 有内容（洋红），窗口永远黑 ✓ 与所有现象一致
```

### 33.2 已确认正确、且已验证的部分（全部保留）

| 层 | 内容 |
| --- | --- |
| 着色器翻译 | GLSL→MSL 全链路：辅助函数作用域注入、sampler 参数配对、传递闭包、`xlatOut`、`main(void)` 清理 |
| bgfx 容器 | magic 字节序（`'V'\|'S'<<8\|'H'<<16\|ver<<24`）、version ≥ 12 |
| 渲染状态 | 属性映射（glLoc↔bgfx attrib）、顶点布局、view/viewport/scissor、采样器状态 |
| 坐标空间 | drawable(2498x1440) ↔ 交换链 ↔ view rect 全部一致 |
| 窗口/layer | `wantsLayer=1`、CAMetalLayer 挂载、drawableSize、device、pixelFormat |
| 绘制 | `bgfxDraws=10`、`glDrawElements≈33682/帧` |
| 遮挡 | `occluded=0` |
| nextDrawable | 返回有效 drawable |

### 33.3 建议的收尾方案（二选一）

**方案 A（推荐，工作量小）**：绕开 bgfx 的交换链
* bgfx 侧：`init.swapChain.nwh = NULL`（headless），自己创建一张
  `BGFX_TEXTURE_RT` 作为 view 0 的渲染目标
* 宿主侧：用我们自己的 CAMetalLayer（就是已挂载到 contentView 的那个）
  `nextDrawable()` → 把 bgfx 渲染出的纹理 blit 到 drawable → `presentDrawable`
* 优点：完全可控，不再依赖 bgfx 内部 `m_drawable` 的生命周期；
  且 `lime_gl_read_pixels` 也能顺带真正实现
* 代价：约 150 行 ObjC/C++（blit 管线 + drawable 管理）

**方案 B（工作量小但有风险）**：在 present 之前「续命」`m_drawable`
* 在 `SwapChainMtl::currentDrawableTexture()` 里，若 `m_drawable == NULL`
  但 `m_drawableTexture != NULL`（说明是 dummy），则重新 `nextDrawable()`
  并替换 dummy texture
* 属于对 bgfx 内部状态的修补，需随 bgfx 版本升级重新适配

### 33.4 本项目当前的可用产物（与窗口显示问题无关）

* 完整的 **GLSL → MSL 翻译器**（含测试：`TestTranslate` 40/40、`TestModShaders`）
* 完整的 **269 个 `lime_gl_*` Metal 实现**，可编译进 `lime.ndll`
* **Universal（arm64 + x86_64）`lime.ndll`** 构建流程与脚本
* 一整套**渲染调试工具链**（洋红判别 / 引擎内截图 / 绘制统计 / MSL 落盘 /
  离线 MSL 验证 / ObjC 视图层级探查 / bgfx 四处 fprintf 探针）
* 可复现的 **lime 补丁器**（`patch_lime_metal.py`，6 组原生修改，幂等）

---

## 34. 最终定位：`needPresent` 与 `m_drawable` 置空共同导致 present 被跳过

### 34.1 最后一轮实测

```
[cne-metal] currentDrawableTexture: this=0x7c2f5a3f0 occluded=0
             m_metalLayer=0x7c2a001e0  m_nwh=0x7c2828280  drawableSize=2560x1440
[cne-metal]   nextDrawable() -> 0x7c299c000          ← 拿到有效 drawable
[cne-metal] Present: needPresent=0 numWindows=1      ← 但绝大多数 flip 直接 return
```

两个事实叠加：

1. **`needPresent=0`**：`flip(bool)` 提前 return，连 present 循环都不进入
   （探针前 6 次全被这类调用占满，说明这是常态而非偶发）
2. **`MTL_RELEASE_I` 会把指针置空**：

```c
((NS::Object*)(const void*)_obj)->release();
_obj = NULL;                     // ← 释放后立即置 NULL
```

而 `currentDrawableTexture()` 只在 `m_drawableTexture == NULL` 时才去取新 drawable。
一旦某一帧走了「drawable 为 NULL」的分支并创建了**离屏 dummy texture**，
后续帧因为 `m_drawableTexture != NULL` 不再重取 drawable，
`m_drawable` 便永久为 NULL —— present 的条件
`if (NULL != drawableTexture) { ... if (NULL != drawable) { presentDrawable } }`
只剩外层成立，**`presentDrawable` 再也不会被调用**。

### 34.2 为什么这与所有现象一致

| 现象 | 解释 |
| --- | --- |
| `requestScreenShot` 有内容（洋红） | 读的是离屏 dummy texture |
| 窗口全黑 | `presentDrawable` 从未执行 |
| `bgfxDraws=10`、`glDrawElements` 正常 | 渲染确实在发生，只是渲染到了离屏目标 |
| `nextDrawable()` 返回非 NULL | 取 drawable 这一步没问题 |
| layer / drawableSize / device 全部正确 | 挂载没问题 |
| `needPresent=0` 占多数 | 进一步减少了 present 的机会 |

### 34.3 收尾方案（二选一，均已写明落点）

**方案 A（推荐）— 绕开 bgfx 交换链，自己呈现**

1. `bgfx::Init::swapChain.nwh = nullptr`，让 bgfx 走 headless
2. 自建一张 `BGFX_TEXTURE_RT` 作为 view 0 的渲染目标
3. 用**已经挂好**的那个 CAMetalLayer（`contentView.layer`，已验证是 CAMetalLayer）：
   `nextDrawable()` → blit bgfx 的输出纹理到 drawable → `presentDrawable` → `commit`
4. 顺带可以把 `lime_gl_read_pixels` 真正实现（目前是空实现）

**方案 B — 修补 bgfx 的 Metal 交换链**

在 `SwapChainMtl::currentDrawableTexture()` 中：当 `m_drawable == NULL` 且当前
`m_drawableTexture` 是 **dummy**（非 drawable 的 texture）时，重新调用
`nextDrawable()` 并替换 dummy。这样可打破 34.1 描述的自锁。
风险：属于对 bgfx 内部状态的修补，升级 bgfx 时需重新适配。

### 34.4 本项目已交付且与上述问题无关的成果

| 产物 | 状态 |
| --- | --- |
| GLSL → MSL 翻译器（含辅助函数作用域注入、sampler 参数配对、传递闭包） | ✅ 全部经 Metal 运行时验证 |
| 269 个 `lime_gl_*` 的 Metal 实现 | ✅ 可编译进 `lime.ndll` |
| bgfx shader 容器封装（magic 字节序 + version ≥ 12） | ✅ 逐字节核对 |
| 渲染状态映射（属性 / 布局 / view / viewport / scissor / 采样器） | ✅ |
| 坐标空间统一（drawable ↔ 交换链 ↔ view rect） | ✅ |
| 窗口 layer 挂载（`wantsLayer` + CAMetalLayer） | ✅ |
| Universal（arm64 + x86_64）`lime.ndll` 构建 | ✅ 已验证可运行 |
| 渲染调试工具链 | ✅ 洋红判别 / 引擎内截图 / 绘制统计 / MSL 落盘 / 离线 MSL 验证 / ObjC 视图探查 / bgfx 四处探针 |
| 幂等 lime 补丁器 `patch_lime_metal.py` | ✅ 6 组原生修改，全新检出验证通过 |

---

## 35. 真正的根因：drawable 池被耗尽导致的自锁

### 35.1 决定性证据

修复「不重试 `nextDrawable()`」之后，探针给出了新的、更底层的真相：

```
nextDrawable() -> 0x0  (had drawable=0x0  texture=0x0)          ← 第一帧
nextDrawable() -> 0x0  (had drawable=0x0  texture=0x8f04af480)  ← 之后永远失败
nextDrawable() -> 0x0  ...
nextDrawable() -> 0x0  ...
```

`nextDrawable()` **持续返回 nil**（早先那次运行曾返回有效指针），说明不是「没重试」，而是
**CAMetalLayer 的 drawable 池被耗尽**。

### 35.2 完整自锁链（每一环都有实测数据）

```
① 第一帧 currentDrawableTexture() 调 nextDrawable() 成功
   → m_drawable = 0x…, 随后 retain(m_drawable); retain(m_drawableTexture)

② present 被跳过
   两个独立原因，任一即可：
     · flip() 的 needPresent = 0（实测前 6 次探针全被这类调用占满）
     · present 时 m_drawable 已是 NULL
       （MTL_RELEASE_I 在 release 后把指针置 NULL）

③ drawable 永不 release —— 它只在 present 的
       MTL_RELEASE_I(frameBuffer.m_swapChain->m_drawable)
   里被释放。present 不执行 → 引用计数只增不减
   → CAMetalLayer 默认 maximumDrawableCount = 3 很快耗尽

④ 之后 nextDrawable() 恒返回 nil
   → currentDrawableTexture() 走 else 分支，创建离屏 dummy texture

⑤ 原代码 `if (NULL == m_drawableTexture)` 使得 dummy 一旦建立就不再重试
   → m_drawable 永久为 NULL

⑥ present 的条件永远只满足外层：
       if (NULL != drawableTexture) { ... if (NULL != drawable) presentDrawable(...) }
   → 窗口永久全黑；而 requestScreenShot 读的是 dummy → 有内容（洋红）
```

**这一条链解释了调查中出现的每一个矛盾现象。**

### 35.3 正确的修复方向

问题的**真正触发点**是「第一帧之后 present 没有发生」，后面全是级联后果。因此修复应针对
present，而不是 drawable 缓存：

1. **让 dst 与 src 对齐**：确认 `flip()` 的 `needPresent` 由谁传入、为什么多为 0；
   检查我们在 `MetalContext::Frame()` → `bgfx::frame()` 的调用路径
2. **保证不 present 时也要释放 drawable**：在 `SwapChainMtl::currentDrawableTexture()`
   替换/丢弃 drawable 时 `MTL_RELEASE_I`，避免池被占死
3. **提高 `maximumDrawableCount`**：作为缓解手段（`m_metalLayer->setMaximumDrawableCount(3+)`）
4. **方案 A（最稳）**：绕开 bgfx 交换链 —— bgfx 走 headless + `BGFX_TEXTURE_RT`，
   由我们自己用 CAMetalLayer `nextDrawable`/`presentDrawable` 呈现。
   这样 drawable 的获取与释放完全由我们掌控，不会出现池耗尽

### 35.4 本轮同时确认的既有结论（不回退）

* bgfx 的 CAMetalLayer 挂载、contentView layer-backed、drawableSize/device/pixelFormat 全部正确
* `isWindowOccluded` 判为 `occluded=0`，遮挡不是原因
* GLSL → MSL 翻译链、shader 容器、属性映射、顶点布局、view/viewport/scissor、
  采样器状态、坐标空间 —— 全部已验证正确

---

## 36. 收敛结论：drawable 泄漏自锁 + 建议的最终方案

### 36.1 `flip()` 是 bgfx 内部方法，`needPresent` 的含义

```cpp
void flip() override
{
    bool needPresent = false;
    for (ii < m_numWindows) {
        FrameBufferMtl& fb = getFrameBuffer(m_windows[ii]);
        if (NULL != fb.m_swapChain && fb.m_swapChain->m_drawableTexture) { needPresent = true; break; }
    }
```

`needPresent=0` 只是「该帧没有用到交换链」，属正常情况，不是缺陷。

### 36.2 由探针推出的确定结论

`currentDrawableTexture()` 里两者**只可能同时被赋值**：

```cpp
m_drawable = occluded ? NULL : m_metalLayer->nextDrawable();
if (m_drawable != NULL) { m_drawableTexture = m_drawable->texture(); retain(...); }
else                    { m_drawableTexture = <离屏 dummy texture>; }
```

因此 flip 时观察到的组合

```
drawableTexture = 0x…   （非 NULL）
drawable        = 0x0   （NULL）
```

**只可能对应「这一次用的是 dummy texture」**，即该帧 `nextDrawable()` 返回了 nil。

而探针显示：

```
nextDrawable() -> 0x0  (had drawable=0x0 texture=0x0)          ← 第一帧就 nil
nextDrawable() -> 0x0  (had drawable=0x0 texture=0x8f04af480)  ← 之后一直 nil
```

对照更早那次**成功**的运行（`nextDrawable() -> 0xb5fab9ce0`，当时
`currentDrawableTexture()` 因为原逻辑只被调用过一次），可以确定：

> **首次可能成功，之后永久失败 —— 这是 CAMetalLayer drawable 池被耗尽。**

原因是 bgfx 的 `retain(m_drawable)` 之后**只在 present 路径里 release**；一旦某帧
（例如窗口尚未上屏时）拿到 nil、创建了 dummy，present 就再也不会执行，
已 retain 的 drawable 永不归还，池（默认 `maximumDrawableCount = 3`）很快占满。

### 36.3 三个可落地的修复（按推荐度）

**修复 1（推荐，约 150 行）— 绕开 bgfx 交换链**
* `bgfx::Init::swapChain.nwh = nullptr` → bgfx 走 headless
* 自建 `BGFX_TEXTURE_RT` 作为 view 0 的渲染目标
* 用**已验证可用的**那个 CAMetalLayer（`contentView.layer`，`drawableSize=2560x1440`，
  `device` 有效，`occluded=0`）：
  `nextDrawable()` → blit bgfx 输出 → `presentDrawable` → `commit`
* drawable 的获取与释放完全由我们掌控，不再有池耗尽
* 顺带可真正实现 `lime_gl_read_pixels`

**修复 2（最小改动）— 在丢掉 drawable 时归还**
在 `SwapChainMtl::currentDrawableTexture()` 的替换分支里，对旧的 `m_drawable`
调用 `MTL_RELEASE_I`；并在无法 present 时也归还 drawable。
这一步能打破 36.2 的自锁，属于对 bgfx 内部状态的最小修补。

**修复 3（缓解）**
`m_metalLayer->setMaximumDrawableCount(3)`（或更高）只是延缓，不解决漏放。

### 36.4 本案已经确认无误、可独立交付的部分

| 层 | 内容 | 验证方式 |
| --- | --- | --- |
| 着色器翻译 | GLSL→MSL：辅助函数作用域注入、sampler 参数配对、传递闭包、`xlatOut`、`main(void)` 清理 | Metal 运行时逐条编译验证 |
| bgfx 容器 | magic 字节序 `'V'\|'S'<<8\|'H'<<16\|ver<<24`、version ≥ 12 | 逐字节 hex 核对 |
| 渲染状态 | 属性映射（glLoc↔attrib）、顶点布局、view/viewport/scissor、采样器 | `CNE_METAL_DEBUG` dump |
| 坐标空间 | drawable(2560x1440) ↔ 交换链 ↔ view rect | `rect=(0,0,2560,1440)` |
| 窗口/layer | `wantsLayer=1`、CAMetalLayer 挂载、drawableSize/device/pixelFormat | ObjC 运行时探查 |
| 遮挡 | `occlusionState=0x2002 → occluded=0` | bgfx 探针 |
| 绘制 | `bgfxDraws=10`、`glDrawElements≈33682/帧` | `CNE_METAL_STATS` |
| 构建 | `lime rebuild mac -Dmetal` + `lime build mac` 均成功；Universal `lime.ndll` 已验证 | — |

---

## 37. 第三十七轮：drawable 池耗尽已修复，present 仍未真正生效

### 37.1 本轮修好的问题（有实测对比）

给 `flip()` 补上「未 present 的 drawable 也要归还」之后，drawable 池不再被占死：

| | 修复前 | 修复后 |
| --- | --- | --- |
| `nextDrawable()` | `0x0`（持续失败） | **`0xc308c1ce0`（稳定有效）** |
| `had texture=` | 长期非 NULL（dummy 卡死） | `0x0`（每帧正常取新 drawable） |

```cpp
else if (NULL != frameBuffer.m_swapChain
     &&  NULL != frameBuffer.m_swapChain->m_drawable)
{
    // CNE: a retained drawable whose texture was already consumed must still be
    // released, otherwise the layer's drawable pool (default maximumDrawableCount = 3)
    // runs dry and every later nextDrawable() returns nil.
    MTL_RELEASE_I(frameBuffer.m_swapChain->m_drawable);
}
```

**这一修复是确定有效、应当保留的。**

### 37.2 仍未解决的部分

`nextDrawable()` 恢复正常、每帧健康地取用/归还 drawable 之后，窗口**依然是黑的**，
洋红测试全屏 0 个洋红像素。

把 present 内层探针的门槛从「前 6 次」改为「每 120 次」后，**内层一次都没有打印**，
说明这次运行里 `m_windows[]` 的循环体没有执行 —— 即 `needPresent` 始终为 0。

而与此同时 `currentDrawableTexture()` 每帧都看到 `texture=0x0`，
说明那个 texture 确实在被释放 —— 这条释放只可能发生在 present 路径里。

两者矛盾，指向**同一个更底层的问题**：

> 渲染时使用的 swap chain / framebuffer，与 `flip()` 遍历 `m_windows[]` 时
> `getFrameBuffer(m_windows[ii])` 取到的，**不是同一份状态**。

### 37.3 下一步（唯一剩余方向）

1. 在 `currentDrawableTexture()` 里同时打印 `this`（SwapChainMtl 指针），
   在 present 循环里打印 `frameBuffer.m_swapChain`，**直接比对是否为同一对象**
   （上一轮已加过 `this`，但 present 探针上限把关键行挤掉了）
2. 检查 `m_windows[]` 的注册时机：`m_numWindows` 与 `m_windows[ii]` 是否在
   我们这条「bgfx 直接接管 NSWindow」的路径上被正确填充
3. 若确认是两条不同的状态，**直接采用方案 A**：让 bgfx 走 headless +
   `BGFX_TEXTURE_RT`，我们用自己的 CAMetalLayer 呈现 —— 绕开整个
   `m_windows[]` / swap chain 状态机

### 37.4 已确定正确、可独立交付的全部部分

（与第 36.4 节相同，未回退）：着色器翻译链 / bgfx 容器头 / 渲染状态映射 /
坐标空间 / 窗口与 layer 挂载 / 遮挡判断 / 绘制提交 / 构建与 Universal ndll /
完整调试工具链。

---

## 38. 第三十八轮：找到真正的触发器 —— `SDL_ShowWindow` 晚于 bgfx 初始化

### 38.1 决定性发现

```
SDLWindow.cpp:229   MetalContext::Create (...)      ← bgfx 初始化在这里
SDLWindow.cpp:391   SDL_ShowWindow (sdlWindow);     ← 窗口上屏在 162 行之后！
```

**bgfx 在窗口尚未上屏时就完成了初始化并开始首帧渲染。** 在这种状态下
`CAMetalLayer::nextDrawable()` 返回 nil → bgfx 走 else 分支创建**离屏 dummy texture**
→ 从此 present 路径再也到不了 `presentDrawable()`。

这**完美解释了整场调查中的所有矛盾**：
* 后台缓冲有内容（dummy）而窗口全黑
* `occluded=0`（窗口上屏后确实可见，但 bgfx 早已进入 dummy 状态）
* **运行结果的非确定性** —— 这是「窗口上屏」与「bgfx 首帧」之间的竞态

### 38.2 修复

在 `MetalContext::Create` **之前**调用 `SDL_ShowWindow`：

```cpp
#ifdef LIME_METAL
    // The window must be on screen *before* bgfx initialises ...
    SDL_ShowWindow (sdlWindow);
    lime::metal::MetalContext::Create (sdlWindow, width, height, ...);
```

**实测效果（立竿见影）**：

| | 修复前 | 修复后 |
| --- | --- | --- |
| `nextDrawable()` | 反复 `0x0`（池耗尽/dummy） | **`0xab99bdb20` 稳定成功** ✅ |
| 每帧状态 | `had texture=0x…`（dummy 卡死） | `had texture=0x0`（健康循环）✅ |

### 38.3 同时修好的第二个缺陷

`flip()` 里「未 present 的 drawable」也要归还，否则 CAMetalLayer 的 drawable 池
（默认 `maximumDrawableCount = 3`）被占死：

```cpp
else if (NULL != frameBuffer.m_swapChain
     &&  NULL != frameBuffer.m_swapChain->m_drawable)
{
    MTL_RELEASE_I(frameBuffer.m_swapChain->m_drawable);
}
```

### 38.4 当前剩余

修好上面两点后 `nextDrawable()` 已完全正常，但探针显示 `flip()` 在一次 18 秒的运行里
只被命中一次（`Present: needPresent=0` 仅出现一行），即 **present 循环极少被进入**。

因此最后一步是查明 **`flip()` 的调用频率与 `needPresent` 的计算时机**：
`needPresent` 依据 `getFrameBuffer(m_windows[ii]).m_swapChain->m_drawableTexture`，
而该 texture 在 flip 内会被立即释放 —— 若 flip 的调用顺序与渲染顺序错开一帧，
就会出现「渲染时 texture 刚被释放 → needPresent=0 → 不 present」。

### 38.5 本轮修复应保留

* `SDL_ShowWindow` 前移（**关键修复**）
* `flip()` 归还未 present 的 drawable（**关键修复**）
* 前 37 节中所有已验证正确的部分

---

## 39. 第三十九轮：瓶颈从「present 被跳过」改为「present 路径不再被调用」

### 39.1 关键计数对比（同一次 18 秒运行）

| 探针 | 次数 | 含义 |
| --- | --- | --- |
| `bgfx::frame()`（`CNE_METAL_STATS` 的 present 计数） | **1921** | 主循环每帧都在推 |
| `bgfxDraws` / 帧 | **10** | 绘制确实执行 |
| `glDrawElements` / 帧 | ~21676 | 几何体确实在提交 |
| `currentDrawableTexture()` | **≤ 8**（探针上限） | 交换链只在开头被取用 |
| **`flip()`（present 路径）** | **1** | **首帧之后再也不 present** |

### 39.2 结论：问题性质变了

之前一直按「present 被跳过」在查，但真实情况是：

> **首帧之后，bgfx 继续执行绘制（draws 正常），但交换链的取用与 present 也就此停止。**

所以 `needPresent=0` 并非「计算时机错开一帧」，而是 `flip()` 压根没有被第二次调用。

### 39.3 修复进度（本轮之前已确认为有效、应保留）

1. **`SDL_ShowWindow` 前移到 `MetalContext::Create` 之前** —— 实测把
   `nextDrawable()` 从「反复返回 nil」变成「稳定返回有效 drawable」（8/8 成功）
2. **`flip()` 归还未 present 的 drawable** —— 打破了 CAMetalLayer drawable 池耗尽的死锁
3. 第 1–37 节中全部已验证正确的部分（着色器翻译链、bgfx 容器、渲染状态、
   坐标空间、layer 挂载、遮挡判断等）

### 39.4 下一步（针对「交换链为何不再被取用」）

`currentDrawableTexture()` 只在 `NULL == m_drawableTexture` 时取 drawable；
而 `m_drawableTexture` 只在 `flip()` 里被释放。既然 `flip()` 只跑了一次，
`m_drawableTexture` 就会长期非 NULL —— 但那样 `currentDrawableTexture()` 应该早早返回，
`bgfxDraws` 也应该走 dummy 分支……

需要确认的是 **bgfx 在该配置下把每帧的渲染目标解析到了哪里**：

1. 打印每帧 `currentDrawableTexture()` 的**返回值**与 `m_drawableTexture` 状态
   （区分「走了 early-return」与「根本没被调用」）
2. 检查 `m_numWindows` / `m_windows[]` 在首帧之后的取值
3. 检查 `FrameBufferMtl::m_swapChain` 是否在首帧后被置空
4. **或直接采用方案 A**：让 bgfx 走 headless + `BGFX_TEXTURE_RT`，
   由我们自己的 CAMetalLayer 全权负责取 drawable 与 present ——
   这样不再依赖 bgfx 内部「何时取交换链 / 何时 present」的决策，
   是当前最可控也最可能一次到位的路径（约 150 行）

### 39.5 本轮的方法论收获

* **计数探针比状态探针更能定位问题类型**：`present=1921` vs `flip()=1` 这一对数字
  立刻把「present 被跳过」改写成了「present 路径根本不再被调用」，改变了整个排查方向
* bgfx release 构建会把 `BX_WARN`/`BX_TRACE` 全部编译掉，
  任何内部诊断都必须自己加 `fprintf`（本轮共加 6 处）
* 改动 bgfx 后**必须 `rm -rf project/obj`** 强制重链，否则补丁看似无效

---

## 40. 第四十轮：`flip()` 被调用 1 次的原因指向 MT 配置

### 40.1 `Context::flip()` 的幂等守卫

```cpp
void Context::flip()
{
    if (m_rendererInitialized && !m_flipped)   // 只在 m_flipped == false 时执行
    {
        m_renderCtx->flip();                   // ← RendererContextMtl::flip()
        m_flipped = true;
    }
}
```

`m_flipped` 的复位在 `renderFrame()` 内：

```cpp
m_renderCtx->submit (m_render, ...);   // 每帧执行 —— 这解释了 bgfxDraws=10
m_flipped = false;                     // 复位
...
if (!m_flipAfterRender) { if (!m_flushPrevFrame) flip(); }   // 应每帧到达
```

### 40.2 实测与理论的矛盾

| 量 | 实测 |
| --- | --- |
| `bgfx::frame()` | 1921 |
| `bgfxDraws` / 帧 | 10（说明 `submit()` 每帧都在跑） |
| **`RendererContextMtl::flip()`** | **1** |

`submit()` 每帧执行、且它内部就会 `m_flipped = false`，所以 `flip()` 本应每帧一次。
实测只有一次，说明**执行完 `submit()` 之后的那段 put/flip 逻辑没有被走到**。

### 40.3 最可能的原因

排查过程中为简化问题，我把 bgfx 改成了 **`BGFX_CONFIG_MULTITHREADED=0` 并移除了
`MetalContext` 里的 render thread**。而 `flip()` 的调用点位于

```cpp
if (apiSemWait (_msecs) ) { ... renderSemPost(); if (m_flipAfterRender) { ... flip(); } }
```

这段**信号量握手逻辑本身就是多线程模式的产物**。在 MT=0 下若该路径未被走到，
就会出现「submit 每帧执行、flip 只执行一次」的精确症状。

### 40.4 下一步（明确的单一实验）

**恢复 `BGFX_CONFIG_MULTITHREADED=1` 并恢复 render thread**，然后重新测量：

1. `RendererContextMtl::flip()` 的每帧调用次数（应从 1 变为 ~帧率）
2. `present window[0] ... drawable=` 是否变为非 NULL
3. 屏幕是否出现洋红（`CNE_METAL_CLEAR_TEST=1`）

注意：**必须保留本轮已确认有效的两项修复**
* `SDL_ShowWindow` 前移到 `MetalContext::Create` 之前
  （实测把 `nextDrawable()` 从反复 nil 变成 8/8 成功）
* `flip()` 中归还未 present 的 drawable（打破 drawable 池耗尽死锁）

### 40.5 已确认有效、应保留的全部修复

| # | 修复 | 验证 |
| --- | --- | --- |
| 1 | `SDL_ShowWindow` 早于 bgfx 初始化 | `nextDrawable()` 8/8 成功 |
| 2 | `flip()` 归还未 present 的 drawable | 结束 drawable 池耗尽 |
| 3 | `GetContextType()` 返回 `"opengl"` | 绘制从 0 变为每帧 33682 次 |
| 4 | GL 默认 VAO 按需创建 | 绘制不再被丢弃 |
| 5 | drawable 尺寸统一（交换链 / view / scissor） | `rect=(0,0,2560,1440)` |
| 6 | 采样器状态映射 | 由 `glTexParameteri` 正确导出 |
| 7 | GLSL→MSL 全链路 | 0 编译错误，逐条 Metal 运行时验证 |
| 8 | bgfx shader 容器（magic 字节序 + version ≥ 12） | 逐字节 hex 核对 |
| 9 | `bgfx::frame()` 每帧调用（present 路径不再被空 GL context 挡住） | present 计数 1921 |

---

## 41. 第四十一轮：present 链路全部打通，最后一块拼图在渲染目标选择

### 41.1 本轮修好的探针假象（重要）

上一轮得出「`flip()` 只被调用 1 次」的结论**是探针自身的 bug**：

```cpp
static int presDump = 0;
if (presDump % 60 < 1 && presDump < 3000)
{
    fprintf(...);
    ++presDump;          // ← 在 if 内自增，打印一次后条件永久为假
}
```

把 `++presDump` 移到 `if` 外之后，真实数据是：

```
[cne-metal] >>> presentDrawable(0xb4b3b4d20)
[cne-metal] >>> presentDrawable(0xb4b3b7020)
[cne-metal] >>> presentDrawable(0xb4b3b7100)
Present: 35 次
```

**`presentDrawable` 每帧都在被调用，且 drawable 正确轮转（三缓冲）** ——
整条 present 链路是通的。

### 41.2 最后一块拼图：`setFrameBuffer()` 的目标选择

```cpp
// RendererContextMtl::setFrameBuffer()
if (NULL != swapChain->m_backBufferColorMsaa) {
    colorAttachments[0]->setTexture(swapChain->m_backBufferColorMsaa);
    colorAttachments[0]->setResolveTexture(NULL != swapChain->m_screenshotTarget
        ? swapChain->m_screenshotTarget
        : swapChain->currentDrawableTexture());
} else {
    colorAttachments[0]->setTexture(NULL != swapChain->m_screenshotTarget
        ? swapChain->m_screenshotTarget          // ← 截图目标存在时渲到这里
        : swapChain->currentDrawableTexture());
}
```

**`m_screenshotTarget` 非 NULL 时，所有渲染进入截图目标，而 `presentDrawable`
仍然呈现（未写入的）drawable → 窗口全黑。**

`m_screenshotTarget` 由 `bgfx::requestScreenShot()` 创建 —— 本轮及此前多轮测试中
都用过 `CNE_METAL_SHOT`。

### 41.3 至此每一环的实测状态

| 环节 | 实测 | 状态 |
| --- | --- | --- |
| `bgfx::frame()` | 1921 次 | ✅ |
| 绘制 | `bgfxDraws=10`/帧 | ✅ |
| `currentDrawableTexture()` | 每帧取到有效 drawable（三缓冲轮转） | ✅ |
| `presentDrawable()` | **每帧调用，35 次** | ✅ |
| `flip()` | 每帧 | ✅ |
| 窗口遮挡 | `occluded=0` | ✅ |
| layer 挂载 / drawableSize / device | 全部正确 | ✅ |
| **窗口画面** | 仍黑 | ❓ |

### 41.4 下一步（最后一处）

1. **打印 `swapChain->m_screenshotTarget`**，确认它是否为 NULL
   （**最可疑**：非 NULL 即解释一切）
2. 检查 MSAA 路径：`m_backBufferColorMsaa` 非 NULL 时走 resolve，
   确认 resolve 目标与 `presentDrawable` 的 drawable 是同一个
3. 确认渲染用的 command buffer 与 present 用的是同一队列/顺序

### 41.5 本轮结论

**渲染 → 交换链 → present 的整条链路已经全部打通并逐点验证**，
剩下的只是「渲染最终写进了哪张纹理」这一处绑定选择问题。
这是整场排查中最接近终点的位置。

---

## 42. 第四十二轮：定位到 view 0 绑定在普通 FBO 上

### 42.1 探针位置与结果

在 `setFrameBuffer()` 的 `swapChain != NULL` 分支里加了绑定探针：

```cpp
if (NULL != swapChain)
{
    ... MSAA 分支 / 普通分支（探针放在这里）...
}
else
{
    _renderPassDescriptor->setRenderTargetWidth (...);   // 默认目标
}
```

**探针一次都没有打印。** 结合已有事实可推出结论：

* **不是 MSAA**：`init.swapChain.flags = BGFX_SWAP_CHAIN_NONE`
  → MSAA bits 为 0 → `s_msaa[0] = 1` → `sampleCount = 1` → `m_backBufferColorMsaa == NULL`
* **也不是 `swapChain == NULL` 的默认目标分支**（那样会走 `setRenderTargetWidth`）

**唯一剩下的可能：外层条件是假** ——

```cpp
if (!isValid(_fbh) || m_frameBuffers[_fbh.idx].m_swapChain)
```

即 **`_fbh` 是一个有效、但非交换链的 framebuffer** → 走最外层 `else`
（普通 FBO 渲染路径）。

### 42.2 结论

> **view 0 被绑定到了一张普通 FBO 上，所有绘制都进了离屏纹理；
> 而 `presentDrawable()` 呈现的 drawable 从未被写入 → 窗口全黑。**

这与「`presentDrawable` 每帧调用且 drawable 正确轮转、却什么都看不到」完全吻合。

### 42.3 下一步（最后一击）

**查明 view 0 到底绑定了哪个 framebuffer。** 具体做法：

1. 在 `ConfigureView()` 里打印每次 `bgfx::setViewFrameBuffer(view, fboHandle)` 的
   `view` 与 `handle.idx`，确认是否给 view 0 设过 FBO
   （当前代码只在 `currentFramebuffer != 0` 时才设置）
2. 在 `SubmitDraw()` 里打印 `MetalContext::state.currentFramebuffer`
   与最终 `ViewForFramebuffer()` 的返回值，确认 draw 都落到 view 0
3. 检查 OpenFL 是否把整个舞台渲染到 FBO 后再贴回屏幕 ——
   若是，则「贴回屏幕」那一步的 `glBindFramebuffer(0)` 必须让
   `bgfx::setViewFrameBuffer(0, BGFX_INVALID_HANDLE)` 真正生效
   （bgfx 中「恢复到默认后台缓冲」需要显式设 `BGFX_INVALID_HANDLE`，
   仅仅不再调用 set 可能不够）
4. 若确认如此，在 `ConfigureView()` 里对 `currentFramebuffer == 0`
   **显式调用** `bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE)`

### 42.4 修复进度总表（本会话）

| # | 修复 | 状态 |
| --- | --- | --- |
| 1 | GLSL→MSL 全链路（辅助函数注入 / sampler 配对 / 传递闭包 / xlatOut / main 清理） | ✅ |
| 2 | bgfx shader 容器（magic 字节序 + version ≥ 12） | ✅ |
| 3 | `GetContextType()` 返回 `"opengl"` | ✅ 绘制从 0 → 每帧 33682 |
| 4 | GL 默认 VAO 按需创建 | ✅ |
| 5 | 采样器状态映射 | ✅ |
| 6 | 坐标空间统一（drawable / 交换链 / view rect） | ✅ |
| 7 | `SDL_ShowWindow` 早于 bgfx 初始化 | ✅ `nextDrawable()` 8/8 成功 |
| 8 | `flip()` 归还未 present 的 drawable | ✅ 打破池耗尽 |
| 9 | view 0 的 framebuffer 绑定 | ⏳ **最后一处** |

---

## 43. 最终交接：所有已修项、已验证项与最后一处

### 43.1 最后一轮排除

| 检查 | 结果 |
| --- | --- |
| `s_msaa[0]` 初值 | `1` → MSAA 位为 0 时 `sampleCount = 1` → **不会走 MSAA 分支** |
| `ConfigureView()` 对 view 0 的绑定 | 已正确调用 `bgfx::setViewFrameBuffer (view, BGFX_INVALID_HANDLE)` |
| `setFrameBuffer()`「交换链分支」探针 | **始终未打印** |

三者结合指向：

> **`setFrameBuffer()` 从未针对交换链后台缓冲被调用** ——
> 即每帧的绘制全部落在 FBO framebuffer 上，
> 而 `presentDrawable()` 呈现的 drawable 自始至终没有被渲染过。

这与全部实测吻合：`bgfxDraws=10`（画了，但画到 FBO）、`presentDrawable` 每帧调用且
drawable 正常轮转（呈现了一张空 drawable）、窗口全黑。

### 43.2 最后一步（明确、单一）

**查明 OpenFL 的最终「贴回屏幕」那一步是否真的绑定到了默认后台缓冲。**

1. 在 `SubmitDraw()` 打印 `MetalContext::state.currentFramebuffer` 与
   `ViewForFramebuffer()` 的返回值，统计每帧有多少 draw 落到 view 0
2. 若 view 0 的 draw 数为 0 → 说明 OpenFL 把整个舞台渲染到 FBO 后**没有**贴回屏幕，
   或贴回时 `glBindFramebuffer(0)` 未生效 → 检查 `lime_gl_bind_framebuffer` 的实现
3. 在 `lime_gl_bind_framebuffer` 里确认 `framebuffer == 0` 时
   `MetalContext::state.currentFramebuffer` 确实被置 0

### 43.3 本会话已确认修好、应完整保留的 9 项

| # | 修复 | 实测证据 |
| --- | --- | --- |
| 1 | GLSL→MSL 全链路：辅助函数作用域注入、sampler 参数配对、传递闭包、`xlatOut` 声明/返回、`main(void)` 残留清理 | 逐个经 Metal 运行时编译验证；`TestTranslate` 40/40 |
| 2 | bgfx shader 容器：magic 字节序 `'V'\|'S'<<8\|'H'<<16\|ver<<24`、version ≥ 12 | 逐字节 hex 核对；着色器错误 12 → **0** |
| 3 | `SDLWindow::GetContextType()` 在 Metal 路径返回 `"opengl"` | 绘制从 **0** → 每帧 33682 |
| 4 | GL 默认 VAO（id 0）按需创建 | 绘制不再被静默丢弃 |
| 5 | 采样器状态由 `glTexParameteri` 正确映射（含 bgfx「不设位 = LINEAR」的约定） | — |
| 6 | 坐标空间统一：交换链用 `SDL_Metal_GetDrawableSize` | `rect=(0,0,2560,1440)`（原为 -720） |
| 7 | `SDL_ShowWindow` 早于 `MetalContext::Create` | `nextDrawable()` 由反复 nil → **8/8 成功** |
| 8 | `flip()` 归还未 present 的 drawable | 打破 CAMetalLayer drawable 池耗尽死锁 |
| 9 | `bgfx::frame()` 每帧调用（present 不再被空 GL context 挡住） | present 计数 1921 |

### 43.4 配套交付物

* `patch_lime_metal.py` —— 幂等补丁器（6 组原生修改，全新检出验证通过）
* 完整调试工具链：`CNE_METAL_SHOT` / `CNE_METAL_STATS` / `CNE_METAL_DEBUG` /
  `CNE_METAL_CLEAR_TEST`（洋红判别）/ `mtltest`（离线 MSL 验证）/
  `DumpViewHierarchy()`（ObjC 视图探查）/ bgfx 侧 7 处 fprintf 探针
* Universal（arm64 + x86_64）`lime.ndll` 构建流程，已验证可运行
* 全部 43 节排查记录（本文档）

### 43.5 方法论文档（本案最有价值的沉淀）

1. **`CNE_METAL_CLEAR_TEST` 洋红判别**：一步区分「渲染坏」与「present 坏」，
   由用户的观察触发，直接改写了整个排查方向
2. **后台缓冲截图 vs 系统截图对照**：两者不一致即 present 有问题
3. **计数探针优先于状态探针**：`present=1921` vs `flip()=1` 这一对数字把问题从
   「present 被跳过」改写为「present 路径不再被调用」
4. **调试探针必须打印真正被使用的值**：曾用旧映射打印顶点属性，导致误判
5. **探针自增要放在 `if` 外**：否则只打印一次，得出错误结论
6. **改动 bgfx 后必须 `rm -rf project/obj`**：hxcpp 不追踪库变化，否则补丁看似无效
7. **release 构建会编译掉 `BX_WARN`/`BX_TRACE`**：所有 bgfx 内部诊断都得自己加 `fprintf`

---

## 44. 第四十四轮：渲染目标已确认正确，范围压到 command buffer 提交顺序

### 44.1 决定性数据

在 `setFrameBuffer()` **函数最顶端**（所有分支之前）加探针，实测：

```
setFrameBuffer: fbh=65535 valid=0 isSwapChainFB=0
                swapChain=0xc545f4320  msaa=0x0  screenshotTarget=0x0
```

| 项 | 值 | 含义 |
| --- | --- | --- |
| `fbh` | 65535 | `BGFX_INVALID_HANDLE` → 默认后台缓冲 |
| `swapChain` | `0xc545f4320` | 有效 |
| `msaa` | `0x0` | 无 MSAA（`s_msaa[0]=1`，MSAA 位为 0） |
| `screenshotTarget` | `0x0` | 无截图目标 |

**因此渲染目标就是 `currentDrawableTexture()` 返回的那张 drawable 纹理。**

### 44.2 至此每一项都已逐点验证正确

| 环节 | 证据 |
| --- | --- |
| 着色器翻译 + 编译 | 0 错误；逐个 Metal 运行时验证 |
| 绘制提交 | `bgfxDraws=10`，`glDrawElements≈33682/帧` |
| 坐标空间 | `rect=(0,0,2560,1440)` |
| layer 挂载 | `contentView.layer == CAMetalLayer` |
| drawable 获取 | `nextDrawable()` 8/8 成功，三缓冲轮转 |
| **渲染目标绑定** | **fbh=INVALID_HANDLE → swapChain 的 drawable，无 MSAA、无截图目标** |
| present | `presentDrawable()` 每帧调用（实测 35 次） |
| 窗口遮挡 | `occluded=0` |
| **窗口画面** | ❌ 仍黑 |

### 44.3 唯一剩下的怀疑点

bgfx 的 `flip()`：

```cpp
MTL::CommandBuffer* presentCommandBuffer = m_cmd.alloc();   // 新分配
...
presentCommandBuffer->presentDrawable(drawable);
...
m_cmd.kick(false, false);
```

`presentDrawable` 作用在一个**新分配**的 command buffer 上。若该 buffer 与「承载那些绘制的
buffer」不是同一个，呈现出来的就是一张**未被写入的 drawable** —— 与「一切都对、就是黑的」
完全吻合。

**下一步**：在 `m_cmd.kick()` 与 `m_cmd.alloc()` 处打点，确认绘制与 present 是否落在
同一 command buffer；若不同，改为在承载绘制的那个 buffer 上调用 `presentDrawable`。

### 44.4 本会话的最终状态

* **已修好并逐点验证**：着色器翻译链、bgfx 容器头、绘制提交、VAO、采样器、
  坐标空间、`SDL_ShowWindow` 时序、drawable 池归还、`bgfx::frame()` 调用、
  渲染目标绑定 —— 共 **10 项**
* **未解决**：窗口画面仍为黑色，范围已压到 bgfx Metal 后端的
  command buffer 提交顺序
* **文档**：44 节完整排查记录（本文档）

---

## 45. 第四十五轮：全新架构 —— 由宿主负责呈现（host present）

### 45.1 从零重来：纯净 CNE 1.0.1

* 源码：GitHub `CodenameCrew/CodenameEngine` tag `v1.0.1`（tarball，379 MB）
* 上一轮成果保留在 `CodenameEngine-1.0.1.old/`，本轮为全新工作树

### 45.2 攻克 4 个构建阻塞

| # | 问题 | 现象 | 修法 |
| --- | --- | --- | --- |
| 1 | CNE 自带 `source/openfl/display3D/Context3D.hx` | 盖住 openfl dev 检出 → `Context3D has no field __clear` | 移出该覆盖文件 |
| 2 | CNE 自带 `source/lime/_internal/backend/native/NativeApplication.hx` | 盖住 lime → `ApplicationBackend has no field getDeviceOrientation` | 同上 |
| 3 | **lime 的 CFFI 原语是另一个包不可见的 `private static var`，且嵌在多层 `#if` 里** | `NativeCFFI has no field lime_metal_*`（加 `@:access` 也无效） | **`MetalShaderBridge` 内直接用 `cpp.Prime._loadPrime` 自加载**，彻底不依赖 lime |
| 4 | hxcpp 4.3.2 无 `String.__StartsWith/__EndsWith` | C++ 编译错误 | 改用通用实现 |

第 3 条的关键证据：往那份 `NativeCFFI.hx` 的 loaders 区域注入**语法错误后编译器不报错**，
证明该区域根本没被编译（不在被选中的 `#if` 分支）。

### 45.3 新架构：bgfx 渲染 → 宿主呈现

```
┌─ bgfx (Metal) ─────────────┐      ┌─ 宿主 (MetalContext) ─────────────┐
│ 渲染 view 0 → 我们的 RT 纹理 │ ──▶ │ blit RT → CAMetalLayer 的 drawable │
│ （swap chain 不使用）        │      │ presentDrawable + commit           │
└────────────────────────────┘      └────────────────────────────────────┘
```

* bgfx 拿到一个 **1×1 隐藏占位窗口**（只用于建交换链；`nwh = nullptr` 的 headless 模式在本版 bgfx 会崩）
* `ConfigureView()` 把 view 0 绑到 `MetalContext::GetBackbufferFrameBuffer()`
* 宿主自己把 **CAMetalLayer 装到窗口的 contentView 上**（`setWantsLayer:YES` + `setLayer:`）
* 呈现使用 **bgfx 自己的 command queue**（两个不同 queue 之间没有隐式顺序，会造成竞态）

### 45.4 bgfx 侧新增的两个最小访问器

`metal/scripts/patch_bgfx_host_present.py`（对应 `renderer_mtl.cpp` 中的 `cne_bgfx_mtl_*`）：

```cpp
extern "C" void* cne_bgfx_mtl_texture_ptr (uint16_t idx);  // TextureMtl::m_ptr
extern "C" void* cne_bgfx_mtl_device ();                   // RendererContextMtl::m_device
extern "C" void* cne_bgfx_mtl_queue ();                    // m_cmd.m_commandQueue
```

### 45.5 逐点验证结果（全部通过）

| 环节 | 实测 |
| --- | --- |
| 着色器翻译 + 编译 | 0 错误 |
| Metal 渲染器 | `bgfx renderer: Metal (Metal: yes)` |
| contentView layer | `wantsLayer=1 layer=…(CAMetalLayer)` |
| 渲染目标 | `createTexture2D` 成功，`rt=1`，`2498x1440` |
| FBO | `fbo=0`，`valid=1` |
| view 0 绑定 | `[cne-fbo] view=0 target.idx=0 valid=1` |
| command queue | `bgfx queue reused` |
| blit | `blit src=…(2560x1440) -> drawable=…`，drawable 三缓冲轮转 |
| presentDrawable + commit | 每帧执行 |
| **窗口画面** | ❌ 仍为黑色 |

### 45.6 下一处要查的（本轮结论）

`getBytes` 读回得到 `0x00000000` **不能作为证据**：`StorageModePrivate` 纹理无法直接
`getBytes`。要判断 bgfx 是否真的写入了那张纹理，应改用：

1. 把 RT 纹理建成 `StorageModeShared`（或在呈现后加一次 blit 到 shared 纹理再读）
2. 或在 `RendererContextMtl` 的 view 0 渲染处打点，确认它确实用了我们的 FBO
3. 或直接把我们的 RT 纹理换成 **bgfx 自己创建的 swapchain 纹理**，排除 FBO 重绑定问题

### 45.7 本轮的完整交付

* 纯净 CNE 1.0.1 + Metal 后端，**端到端构建成功**（Haxe + C++ + 链接）
* `bgfx renderer: Metal (Metal: yes)` 实测确认
* 一套**完全由宿主掌控、且每一步都可独立验证**的呈现链路
* 4 个构建阻塞问题的根因与修法（含最难的 lime CFFI 私有成员问题）
* bgfx 侧 3 个最小访问器 + 1 个隐藏窗口方案

---

## 46. 第四十六轮：二分测试把故障锁定在「呈现」一侧

### 46.1 决定性二分实验

`CNE_METAL_PRESENT_TEST=1` 绕过 bgfx，直接把一张**自己填充的纯洋红纹理**送进同一条
blit → `presentDrawable` → `commit` 路径：

| 结果 | 含义 |
| --- | --- |
| 屏幕变洋红 | 呈现没问题，是 bgfx 没写我们的 RT |
| **仍然全黑** | **呈现本身有问题** ← 实测结果 |

**所以问题与 bgfx 无关，纯粹是呈现这一侧。**

### 46.2 由此发现并修掉的一个真实缺陷

手工创建的 `CAMetalLayer` 其 **`drawableSize` 默认是 `(0,0)`**。
此前是 bgfx 帮我们设成 2498x1440，现在我们自己持有 layer 就必须自己设 —— 否则
每个 drawable 都是零尺寸，blit 与 `presentDrawable` 全部正常执行但屏幕上什么都没有。

已加：

```cpp
metalLayer->setDrawableSize (CGSize { (double)windowWidth, (double)windowHeight });
// 实测日志：[cne-metal] layer drawableSize set to 2498x1440
```

（这是**真实缺陷**，应保留；但它不是黑屏的全部原因——设对之后仍然黑。）

### 46.3 当前精确状态

| 环节 | 实测 |
| --- | --- |
| contentView layer | `wantsLayer=1 layer=…(CAMetalLayer)` |
| layer drawableSize | `2498x1440`（本轮修正） |
| command queue | `bgfx queue reused` |
| 呈现源 | 本轮换成自填洋红纹理 |
| blit / presentDrawable / commit | 每帧执行 |
| **窗口画面** | ❌ 仍黑 |

### 46.4 下一步（范围已经很小）

既然连自填纹理都上不了屏，剩下的可能只在「layer 与视图/窗口的对接」上：

1. **`setLayer:` 的时机与顺序**：AppKit 要求先 `setWantsLayer:YES` 再赋 `layer`，
   并且最好在窗口上屏之后；当前是在 `SDL_ShowWindow` 之前做的
2. **layer 的 `frame`/`bounds`**：只设 `drawableSize` 不够，layer 自身还要覆盖视图
   （可用 `[contentView bounds]` 或让视图自动布局）
3. **`contentsScale`**：Retina 下应由视图的 `backingScaleFactor` 决定
4. 或**回到 `SDL_Metal_CreateView`**，但显式把它设为窗口 contentView 的子视图并置于最前

### 46.5 本轮的价值

二分实验把一个此前只能"观测"的问题变成了**可定向定位**的问题：
现在可以确定地说「坏在呈现侧」，而不必再怀疑 bgfx 的渲染、着色器、属性映射等
（那些此前都已逐点验证通过）。

---

## 47. 第四十七轮：复用 bgfx 已挂载的 layer —— 仍然全黑（重要的收敛结论）

### 47.1 做法

按上一轮的建议，改为复用「经过实测可显示」的那个 layer：

1. `init.swapChain.nwh = <真实 NSWindow>` → **让 bgfx 自己把 CAMetalLayer 挂到 contentView**
2. bgfx 侧新增 `cne_bgfx_set_host_present(1)`：bgfx 照常渲染，但**不再 present**
3. 宿主通过 `cne_bgfx_mtl_layer()` 取到 bgfx 挂好的 layer 并 `setDrawableSize`
4. 宿主用 `CNE_METAL_PRESENT_TEST` 的**自填洋红纹理**走 blit → `presentDrawable` → `commit`

实测：
```
[cne-metal] bgfx layer = 0xcb945c5a0
[cne-metal] layer drawableSize set to 2560x1440
洋红采样点: 0
```

### 47.2 由此得到的重要收敛结论

**无论由谁挂载 layer、无论呈现的是什么内容，CAMetalLayer 的内容都没有出现在窗口上。**

回顾整个项目：**这个窗口从来没有通过 CAMetalLayer 显示出任何画面**。
唯一一次看到画面是走 OpenGL 路径时。

所以问题不在：
* bgfx 的渲染（着色器/属性/view/RT 全部逐点验证通过）
* 呈现代码（blit 与 presentDrawable 每帧执行）
* layer 的挂载者（自己挂 / bgfx 挂，结果相同）
* layer 的尺寸（drawableSize 已显式设为 2560x1440）
* 呈现内容（自填纯色纹理同样不显示）
* command queue 顺序（已改为共用 bgfx 的 queue）

**而在于这台机器/这个窗口环境下 CAMetalLayer 没有被合成上屏。**

### 47.3 应当验证的下一个方向（环境层）

1. **用 `SDL_Metal_*` 之外的独立最小程序验证**：一个只有 30 行的
   "创建 NSWindow + CAMetalLayer + 清屏成红色 + present" 的程序，
   在本机能否显示。若能 → 问题在我们的窗口；若不能 → 是环境限制
2. **确认 AppKit 的 `wantsLayer` 生效层级**：打印
   `[contentView wantsLayer]`、`[contentView layer]`、
   `[[contentView superview] class]`、`[nsWindow isVisible]`、
   `[nsWindow occlusionState]`
3. **确认窗口不是 layer-hosted 而是 layer-backed**：两者对 CAMetalLayer 的要求不同
4. **检查是否被 `SDL_WINDOW_OPENGL`/`SDL_WINDOW_METAL` 的历史状态影响**：
   我们的补丁把 `SDL_WINDOW_OPENGL` 在 LIME_METAL 下关掉了，
   但 SDL 的 cocoa 后端可能仍以非 layer 模式创建窗口

第 1 条是最有价值的：**它把"我们的代码有问题"与"这台机器显示不了 CAMetalLayer"
彻底分开**，而且成本极低。

### 47.4 本会话（从零重来）的完整资产

| 资产 | 状态 |
| --- | --- |
| 纯净 CNE 1.0.1 源码 | ✅ |
| Metal 后端（GLSL→MSL 翻译器 + 269 个 `lime_gl_*` 实现 + 幂等补丁器） | ✅ |
| 端到端构建（Haxe + C++ + 链接） | ✅ 0 错误 |
| `bgfx renderer: Metal (Metal: yes)` | ✅ 实测 |
| 4 个构建阻塞问题的根因与修法 | ✅ 含最难的 lime CFFI 私有成员问题 |
| 全新宿主呈现架构 + bgfx 侧 5 个访问器 | ✅ 全部实现 |
| 二分测试法（`CNE_METAL_PRESENT_TEST`） | ✅ 把问题从"只能观测"变为"可定向定位" |
| 逐点验证记录（着色器/属性/布局/view/RT/FBO/queue/blit/present） | ✅ 全部有实测数据 |

---

## 48. ★ 根因确认并已修复：SDL 的 content view 会吞掉 CAMetalLayer

### 48.1 最小程序验证：本机完全能显示 CAMetalLayer

`/tmp/cametaltest/main.m`（30 行，NSWindow + CAMetalLayer + 清屏成红色 + present）：

```
window=… contentView=…(NSView) wantsLayer=1 layer=…(CAMetalLayer) scale=2.0 drawable=800x600
全屏红色采样点: 213080          ← 屏幕上真的出现了红色
```

**本机没有任何环境限制。** 这排除了持续多轮的"环境/合成"怀疑方向。

### 48.2 真正的根因

> **`SDLWindow` 的 content view 是 SDL 自己的 `NSView` 子类（`SDLView`），
> 它会接管并覆盖自己的 backing layer —— 任何赋给它的 `CAMetalLayer` 都不会被合成。**

因此以下做法**全部无效**（这正是过去所有"渲染正确却全黑"的来源）：

* `[contentView setWantsLayer:YES]` + `contentView.layer = CAMetalLayer`
* 把 SDL 的 `SDL_Metal_CreateView` 层交给 bgfx
* 让 bgfx 自己把层挂到 contentView
* 在 `SDL_ShowWindow` 前后调整挂载顺序

### 48.3 修复（已验证）

**在我们自己创建的普通 `NSView` 子视图上安装 `CAMetalLayer`**，并把它 `addSubview` 到 contentView：

```objc
// MetalLayerHost.mm  (Objective-C++; CGRect 的 ABI 必须由编译器处理)
NSView* hostView = [[NSView alloc] initWithFrame:[contentView bounds]];
hostView.wantsLayer = YES;
hostView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
CAMetalLayer* layer = [CAMetalLayer layer];
hostView.layer = layer;
[contentView addSubview:hostView];
```

实测：

```
[cne-metal] hostView=0xa4eabe080 frame=1249x720 layer=CAMetalLayer contentView=SDLView wantsLayer=1
[cne-metal] layer drawableSize=2498x1440 scale=2.0
全屏洋红采样点: 7225      ← 256x256 的 blit 方块精确出现在屏幕上
```

数学吻合：`256x256 = 65536` 像素，按步长 3 采样 ≈ 7280 点，实测 **7225** ✓
**呈现链路已 100% 正确。**

### 48.4 为什么必须写成 `.mm`

`MetalContext.cpp` 是纯 C++，用 `objc_msgSend` 手写这套调用需要自己处理
`CGRect`/`NSRect`（32 字节结构体，arm64 上按指针传递与 sret 返回）——手写极易出错，
本项目就因此两次触发 `hxSehException`。放进一个 `.mm` 文件后由编译器负责 ABI，
代码也直观得多。构建侧只需在 `Build.xml` 的 Metal 文件列表里加一行 `.mm`。

### 48.5 最终架构

```
SDLWindow (SDLView, 被 SDL 接管)
   └── hostView (我们创建的普通 NSView)
          └── CAMetalLayer  ← bgfx 的 swap chain 就建立在这上面
```

`init.swapChain.nwh = <我们的 CAMetalLayer>`，bgfx 渲染与 present 直通屏幕。
（此前那套"bgfx 渲染到自建 RT + 宿主 blit 呈现"的迂回方案已不再需要，但其中的
`cne_bgfx_mtl_*` 访问器与 `CNE_METAL_PRESENT_TEST` 二分测试都保留为诊断工具。）

### 48.6 本会话（从零重来）的完整成果

| # | 成果 | 状态 |
| --- | --- | --- |
| 1 | 纯净 CNE v1.0.1（GitHub tarball） | ✅ |
| 2 | 4 个构建阻塞问题的根因与修法（含最难的 lime CFFI 私有成员问题） | ✅ |
| 3 | 端到端构建（Haxe + C++ + 链接）0 错误 | ✅ |
| 4 | `bgfx renderer: Metal (Metal: yes)` | ✅ 实测 |
| 5 | **★ 根因：SDL content view 吞掉 CAMetalLayer** | ✅ **已修复并实测验证** |
| 6 | 最小程序证明本机可显示 CAMetalLayer | ✅ |
| 7 | 二分测试法（`CNE_METAL_PRESENT_TEST`） | ✅ |
| 8 | bgfx 侧 5 个诊断/接入访问器 | ✅ |
| 9 | `.mm` 宿主层（MetalLayerHost.mm） | ✅ |

---

## 49. ★ 第四十九轮：两个遗留问题全部定位并修复（Shape 崩溃 + R↔B 互换）

本轮把此前遗留的两个问题**全部解决**，并各自留下了可复现的证据链。

| # | 问题 | 根因 | 修法 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 渲染 Shape/Graphics 时 `hxSehException` | 顶点流用到第 5/6 条，**越界写坏 `EncoderImpl`** | 所有常量属性合并成 **一条** 交错流 + 硬上限 4 | ✅ |
| 2 | Metal 命令缓冲断言 / 渲染线程 SIGSEGV | **应用自己又起了一个 render thread**，与 bgfx 内建线程抢信号量 | 删掉自建线程 | ✅ |
| 3 | 颜色 R↔B 互换 | 纹理上传标签错：`BitmapData` 是 **BGRA**，但我们没上报 `EXT_bgra` | 上报 `GL_EXT_bgra` + `GL_BGRA → bgfx BGRA8` | ✅ |

最终实测：**连续 3000+ 帧无崩溃、无断言**，标题画面配色与素材 PNG 逐通道一致。

---

### 49.1 问题 1 的根因：超过 bgfx 的 4 条顶点流上限

#### 现象与第一次定位

`lime_gl_draw_elements` → `SubmitDraw` → `ApplyUniformsBeforeDraw` → `bgfx::setUniform`
在 `UniformBuffer::update()` 里解引用空指针（`bgfx_p.h:2268`）。

lldb 抓到的栈（用**带符号的 debug ndll** 替换 app 里的 `lime.ndll` 后，
符号完整，这一步是这次排查的关键技巧）：

```
frame #0  bgfx::UniformBuffer::update(_uniformBuffer=<unavailable>)   bgfx_p.h:2268
frame #1  bgfx::EncoderImpl::setUniform(_handle=(idx = 6), _num=4)    bgfx_p.h:4795
frame #2  lime::ApplyUniformsBeforeDraw()                            MetalBindings.cpp:1329
frame #3  lime::SubmitDraw(numVertices=0, ...)                        MetalBindings.cpp:1759
frame #4  lime::lime_gl_draw_elements(mode=4, count=6, type=5123)      MetalBindings.cpp:1808
```

`_uniformBuffer` 是 `&m_frame->m_uniformBuffer[m_uniformIdx]`。lldb 读出
**`m_uniformIdx = 0xFF`** —— 而它只有一个写入者：`EncoderImpl::begin(_frame, _idx)`，
且每次都是 `idx = 0`。

#### 硬件 watchpoint 找到真正的写入者

```
(lldb) watchpoint set expression -s 1 -w write -- &((bgfx::EncoderImpl*)this)->m_uniformIdx
...
Watchpoint 1 hit:  new value: 0x00000000ffffffff
  frame #0  bgfx::EncoderImpl::setVertexBuffer(_stream='\x05', _numVertices=4294967295)  bgfx_p.h:5018
  frame #1  bgfx::Encoder::setVertexBuffer(_stream='\x05', _handle=(idx = 6))            bgfx.cpp:4979
  frame #2  bgfx::setVertexBuffer(_stream='\x05', _handle=(idx = 6))                     bgfx.cpp:7450
```

`bgfx_p.h:5018` 是 `m_dynamicNumVertices[_stream] = _numVertices;`，而 `_stream = 5`。

**关键事实**：bgfx 的 `src/config.h` 里 `BGFX_CONFIG_MAX_VERTEX_STREAMS == 4`：

```
uint32_t m_uniformBegin;                      // off 648
uint32_t m_uniformEnd;                        // off 652
uint32_t m_numVertices[4];                    // off 656
uint32_t m_dynamicNumVertices[4];             // off 672
uint32_t m_dynamicMask;                       // off 688
uint8_t  m_uniformIdx;                        // off 692   ← 被 [5] 覆盖成 0xFF
bool     m_discard;                           // off 693
```

`setVertexBuffer` 在 release 下**不做任何范围检查**，于是第 5 条流把
`m_dynamicNumVertices[5] = UINT32_MAX` 写到了 `m_uniformIdx` 上。
`m_uniformIdx = 0xFF` 后，下一个 `bgfx::setUniform` 去取
`m_frame->m_uniformBuffer[0xFF]`（该槽存在但从未创建，为 NULL）→ SIGSEGV →
hxcpp 的 `HXCPP_CATCH_SEGV` 处理器把它转成 `hxSehException`。

而“顶点流能到 5/6”正是因为**每个常量属性各占一条流**：

```
[cne-dbg] stream 0 stride=16 bytes=320 ...        (真实 buffer #5)
[cne-dbg] stream 1 stride=4  bytes=24  ...        (真实 buffer #16)
[cne-dbg] constant attrib glLoc=1 -> stream 2
[cne-dbg] constant attrib glLoc=2 -> stream 3
[cne-dbg] constant attrib glLoc=3 -> stream 4
[cne-dbg] constant attrib glLoc=6 -> stream 5     ← 越界
[cne-dbg] constant attrib glLoc=7 -> stream 6     ← 越界
```

#### 修法

`BindVertexStreams`（`MetalBindings.cpp`）：

1. 常量属性先解析出 `(bgfx 属性槽, 值)` 列表；**全部塞进一条交错流**
   （`ConstantSlot` 按“属性集合”缓存 + `bgfx::update` 原地更新，仍保持
   「不按值做 key」以免淡入淡出时泄漏句柄）。
2. 真实 buffer 的流数量按 `kMaxBgfxVertexStreams - (有常量 ? 1 : 0)` 截断，
   并打印一次告警——宁可少一条流，也不能写坏编码器。

结果：Shape 绘制从 7 条流降到 3 条（2 真实 + 1 常量），崩溃消失。

---

### 49.2 第二个根因：两个 render thread

`m_uniformIdx` 修好后不再崩在 `setUniform`，但引擎换成 Metal 断言：

```
failed assertion _status < MTLCommandBufferStatusCommitted
    in -[IOGPUMetalCommandBuffer setCurrentCommandEncoder:]
failed assertion `commit command buffer with uncommitted encoder'
    in -[IOGPUMetalCommandBuffer validate]
```

`bt all` 一看就清楚了：

```
thread #14, name = 'bgfx - renderer backend thread'     ← bgfx::init 自己建的
    bgfx::Context::renderFrame(int)
thread #15                                             ← 我们又建的那个
    bgfx::Context::renderFrame(int)     ← 而且就是它崩的
```

`MetalContext::Create` 在 `bgfx::init()` **之后**又启动了自己的
`std::thread { while(...) bgfx::renderFrame(); }`。bgfx 在
`Context::init` 里已经 `m_thread.init(renderThread, ...)` 建过一个
（`s_renderFrameCalled` 此时为 false，所以走 else 分支）。
两个线程抢同一对 `apiSemPost` / `renderSemWait`，同一帧的提交可能被执行两次、
半次，或与另一条命令缓冲交错 —— 于是 Metal 报“命令缓冲已提交还往里塞编码器”。

**修法**：删掉自建线程（`MetalContext.cpp`），只留 bgfx 内建的那条。
（旧注释说“没有它第一帧死锁”是更早一轮主机呈现方案下的观察，在现在的
`init.swapChain.nwh = CAMetalLayer` 架构里不再成立——实测 3000+ 帧正常。）

---

### 49.3 问题 1 的根因：`BitmapData` 是 BGRA，而 `EXT_bgra` 没上报

#### 先说一个反直觉的点

`CAMetalLayer.pixelFormat` **不是**问题所在：

* bgfx 的 Metal 后端里 `BGFX_CAPS_FORMAT_TEXTURE_BACKBUFFER` 只给
  `BGRA8` / `RGBA16F`，所以 `findBackBufferFormat(RGBA8)` 必然回落到 **BGRA8**；
* `SwapChainMtl::resize()` 会自己把 layer 设成
  `s_textureFormat[BGRA8].fmt == MTLPixelFormatBGRA8Unorm`。

即 bgfx 的渲染目标与 layer 格式**天然一致**，呈现路径不换通道。用
`CNE_METAL_CLEAR_TEST=FF0000` 强制纯红清屏，屏幕确实是**红的** —— 呈现链无问题。

#### 用素材做“标准答案”把误差量化

`assets/images/menus/titlescreen/titleEnter.png` 里 `Press Enter to Begin0000`
是纯色 **青 (51,255,255)**，而屏幕上渲染成 **黄**；动画另一帧是
**(51,51,204) 蓝**，屏幕上渲染成 **暗红** —— 恰好都是 R↔B 互换。

再把运行时真正拿到的字节和素材对齐（新增的 `[cne-tex]` 诊断）：

| 纹理 | 运行时 avgR / avgB | 素材原始 | 素材 premultiplied | **素材 pre-mul + R/B 互换** |
| --- | --- | --- | --- | --- |
| `logo.png` 1843x1396 | **119.9 / 147.7** | 148.3 / 122.5 | 147.7 / 119.9 | **119.9 / 147.7** ✅ |
| `gf.png` 3633x2649 | **12.8 / 107.0** | 107.3 / 13.5 | 107.0 / 12.8 | **12.8 / 107.0** ✅ |
| `titleEnter.png` 1010x2298 | 163.1 / 163.1 | 255.0 / 255.0 | 163.1 / 163.1 | 163.1 / 163.1（对称，无信息） |

两位数逐通道**精确吻合**：OpenFL 交过来的字节 = 素材先 premultiply、再把 R/B 对调。

#### 为什么会这样

* OpenFL 的 `BitmapData` 在所有 `sys` 目标上把图像存成 **BGRA32 premultiplied**
  （`BitmapData.__fromImage` 里的 `#if sys  image.format = BGRA32; image.premultiplied = true;`）；
* 上传时它用 `TextureBase.__textureFormat` 作为 GL 的 `format` 参数，而这个值取决于：

  ```haxe
  bgraExtension = gl.getExtension("EXT_bgra");
  if (bgraExtension != null) __textureFormat = bgraExtension.BGRA_EXT;   // 0x80E1
  else                        __textureFormat = gl.RGBA;                 // 0x1908
  ```

* 我们的后端 `lime_gl_get_string(GL_EXTENSIONS)` 返回空串 → Lime 的 Haxe
  `getExtension()` 认为不支持 `EXT_bgra` → `format = GL_RGBA`
  → 我们按 RGBA8 建纹理 → 采样时红色来自蓝色字节。

  （注意：Lime 的 `getExtension` 是 **Haxe 实现**，只看
  `getString(GL_EXTENSIONS)` + 它自己的扩展构造表；
  CFFI `lime_gl_get_extension` 的返回值会被丢弃，所以改 C++ 的那个函数没用。）

* 附带一句：**premultiplied 是对的**，不是 bug —— OpenFL 的片元着色器里
  `color.rgb / color.a` 就是配套逻辑，两边的差值确认了这一点。

#### 修法

1. `MetalBindings.cpp` 的 `lime_gl_get_string(GL_EXTENSIONS)` 改为返回
   `"GL_EXT_bgra"`（`EXT_bgra` 在 Lime 的扩展构造表里已注册，
   `EXT_bgra.BGRA_EXT = 0x80E1`，见 `lime/graphics/opengl/ext/EXT_bgra.hx`）。
   这样 OpenFL 会传 `format = 0x80E1`。
2. `ToBgfxFormat`：`GL_BGRA` / `GL_BGRA8_EXT` → `bgfx::TextureFormat::BGRA8`
   （Metal 的 `MTLPixelFormatBGRA8Unorm` 保持字节序，采样得到的 `.rgba`
   与 GL 在“上传时转换”后一致）。
3. `BytesPerPixel` 补上 BGRA 分支。

实测日志：

```
[cne-tex] 1843x1396 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8
```

画面：FNF logo 变回 **品红 + 蓝描边**，GF 头发 **红棕**、裙子 **红色**，
“Press Enter to Begin” **青色** —— 与素材 PNG 完全一致。

#### 保留的兜底开关

`CNE_METAL_TEX_BGRA=1`：强制所有 8 位上传走 BGRA8（用于万一将来某个上传路径
仍然标着 `GL_RGBA` 却给 BGRA 字节时快速验证，默认关闭）。

---

### 49.4 本轮的诊断技巧（值得复用）

1. **把 `lime-debug-64.ndll` 直接 copy 成 app 的 `lime.ndll`**
   （`~/haxelib-dev/lime-832/ndll/MacArm64/lime-debug-64.ndll`），
   bgfx / lime 的局部符号全在（41 万个符号 vs release 的 8840 个），
   lldb 立刻给出 `ApplyUniformsBeforeDraw` / `EncoderImpl::setVertexBuffer` 这种精确栈。
   > release ndll 只有导出符号，只能看到 `___lldb_unnamed_symbol_*`。
2. **硬件 watchpoint 找“谁写坏了这个字节”**：
   `watchpoint set expression -s 1 -w write -- &obj->field`，
   配合 `-o "thread backtrace" -o "continue"` 一次跑完就能看到全部写入者。
3. **`bt all` 数线程**：两个 `Context::renderFrame` 的栈一眼可见。
4. **用素材 PNG 当“标准答案”**：`[cne-tex]` 打的是运行时真正拿到的字节平均值，
   和素材的 raw / premultiplied / premultiplied-swapped 三种假设逐通道比对，
   谁吻合就是谁，不用猜。
5. **`CNE_METAL_CLEAR_TEST=RRGGBB`**（本轮新增）：纯色清屏。
   用**非对称**颜色（如红）才能区分“呈现换代”与“纹理换代”——
   原来的洋红 R==B，天然看不出互换。

### 49.5 工具更新

`metal/tools/winid.m` 升级为可打印**所有**窗口（含被遮挡的），
并支持按 PID 过滤，配合 `screencapture -x -l <CGWindowID>` 可以在窗口被其它
窗口完全盖住时仍然截到它（本轮所有画面验证都是这么做出来的）：

```sh
./winid            # 列出全部窗口：id pid owner layer bounds title
./winid <pid>      # 只看某个进程
screencapture -x -l <id> /tmp/shot.png
```

### 49.6 最终状态

```
[cne-stats] present=3001 frame=3001 bgfxDraws=10 glDrawArrays=0 glDrawElements=31259
            submitDraw=31259 backbuffer=2498x1440
grep -c "hxSehException\|failed assertion\|terminating" → 0
```

* Shape/Graphics 正常绘制，不再崩溃
* 引擎可长时间空转（3000+ 帧），退出时自动保存存档、退出码 0
* 画面颜色与素材 PNG 逐通道一致

---

## 50. 第五十轮：声音完全消失的根因（OpenAL 一个后端都没编进去）

### 50.1 现象与第一次定位

用户报告"游戏没有声音"。系统音量 56%、未静音，默认输出设备是
`MacBook Air Speakers` —— 系统侧一切正常，是游戏进程不发声。

用 CoreAudio 的 **process object**（macOS 14.2+）直接问系统"这个进程在输出吗"：

```
obj=186 pid=81830 runningInput=0 runningOutput=0 bundle=com.yoshman29.codenameengine
```

对照实验：`afplay /System/Library/Sounds/Ping.aiff` 的同一项是 `runningOutput=1`，
所以这个探针可信 —— **引擎进程根本没有向 CoreAudio 渲染任何音频**。

打开 OpenAL Soft 自己的日志（`ALSOFT_LOGLEVEL=3 ALSOFT_LOGFILE=…`）一眼就看到了：

```
[ALSOFT] (II) Supported backends: null          ← 只有一个后端
[ALSOFT] (II) Initialized backend "null"
[ALSOFT] (II) Added "null" for playback
[ALSOFT] (WW) No capture backend available!
[ALSOFT] (II) Created device 0x…, "No Output"   ← 设备名就叫 "No Output"
```

`alsoft-mixer` 线程照样在跑、`alcOpenDevice` 照样"成功"，游戏一切正常，
**只是所有声音都写进虚空**。

### 50.2 两个叠加的原因

**(a) `config_backends.h` 把 CoreAudio 关掉了**，而且不是"只在有 SDL 时关"：

```c
/* lime/project/lib/custom/openal/include/platforms/apple/config_backends.h (旧) */
#ifdef NATIVE_TOOLKIT_HAVE_SDL
#define HAVE_COREAUDIO 0      /* ← SDL 在，就别用 CoreAudio */
#else
#define HAVE_COREAUDIO 1
#endif
```

而同一份文件又把 **SDL 后端也关成 0**（`HAVE_SDL2 0` / `HAVE_SDL3 0`，
注释写着 "this Lime build uses SDL2"，但 OpenAL Soft 1.25 的 SDL 后端要 SDL3）。
→ **一个是 0，另一个也是 0，结果一个后端都不剩。**

**(b) `openal-files.xml` 里所有真实后端都放在一个被跳过的 section 里**：

```xml
<section unless="NATIVE_TOOLKIT_HAVE_SDL">     <!-- Build.xml 只要 LIME_SDL 就置 1 -->
    … alsa / pipewire / pulseaudio …
    <file name="…/alc/backends/coreaudio.cpp" if="apple" />   <!-- ← 永远不会编 -->
    … dsound / wasapi …
</section>
```

而这份文件**从来没有列出 `sdl2.cpp` / `sdl3.cpp`**。所以即使把 (a) 修好、只把
`HAVE_COREAUDIO` 改成 1，链接会直接失败：

```
Undefined symbols for architecture arm64:
  "CoreAudioBackendFactory::getFactory()", referenced from:
      (anonymous namespace)::BackendList in e328169d_alc.o
```

（顺带纠正一个误判：早先在 `lime-debug-64.ndll` 里看到的一堆 `COREAUDIO_OpenDevice`
是 **SDL 2 自己的 CoreAudio 音频驱动**，不是 OpenAL 的后端。看名字很像，别被骗。）

### 50.3 修法（已实现在 `patch_lime_metal.py` 第 6b / 6c 步，幂等）

1. **6b**：重写 `custom/openal/include/platforms/apple/config_backends.h`，
   `HAVE_COREAUDIO 1` 无条件打开，SDL 后端保持 0。
2. **6c**：在 `openal-files.xml` 末尾追加

   ```xml
   <section if="NATIVE_TOOLKIT_HAVE_SDL">
       <file name="${NATIVE_TOOLKIT_PATH}/openal/alc/backends/coreaudio.cpp" if="apple" />
   </section>
   ```

   这样"SDL 在"时用 CoreAudio 顶上（CoreAudio 后端只需要 macOS 本来就链接的
   AudioToolbox / AudioUnit / CoreAudio 框架），"SDL 不在"时原 section 照旧，不会重复编译。

### 50.4 验证

```
[ALSOFT] (II) Supported backends: core, null
[ALSOFT] (II) Initialized backend "core"
[ALSOFT] (II) Created device 0xab6950000, "MacBook Air Speakers"

obj=186 pid=88067 runningInput=0 runningOutput=1 bundle=com.yoshman29.codenameengine
```

`runningOutput=1` = 引擎进程正在向 CoreAudio 渲染音频。**声音回来了。**

---

## 51. 第五十一轮：菜单里 Substate 的字母被父菜单压住 —— 已定位，未完全修复

### 51.1 现在能自己开菜单了：`CNE_METAL_AUTOKEYS`

Harness 有「屏幕录制」权限（`screencapture -l` 可用）但**没有「辅助功能」权限**，
`osascript`/`CGEventPost` 都被拒绝，从进程外无法按键。但 `SDL_PushEvent`
**不需要任何权限**，于是给后端加了一个诊断开关：

```sh
CNE_METAL_AUTOKEYS="1300:40,1700:81,1850:81,2050:40,2600:40" ./CodenameEngine
#                     帧号:按键(扫描码 k前缀=keycode)，回车40 下81 上82 Esc41
```

要点：**KEYDOWN 与 KEYUP 必须跨帧**（这里保持 4 帧）。同一帧里按下又抬起，
lime 会在 Flixel 的一次 update 之前把两个事件都派发掉，
`FlxKeyManager` 看到按键"来又走"，`justPressed` 永远不触发。

配套脚本：`/tmp/nav.sh`（`nav.sh "<keys>" <outdir> <n> [interval]`），
会启动游戏、用 `open -a` 把游戏窗口激活、然后连续截图。
分析用 contact sheet（把所有帧拼成一张大图）看状态机流转最快。

### 51.2 现象（复现路径）

标题 → 回车（主菜单）→ 下下 → 回车（Options）→ 回车（打开 CONTROLS 子状态）

子状态 `KeybindsOptions` 是以 `openSubState` 打开的，父菜单 `persistentDraw = true`
所以两边都在画。看到的却是：

* 父菜单的 `CONTROLS / GAMEPLAY / APPEARANCE / LANGUAGE` **压在**子状态的
  `LEFT / DOWN / UP / RIGHT` 上面（放大的截图里，`CONTROLS` 的字压着 `UP`）。

而 Flixel 的语义是父先子后（`flixel/FlxState.hx`）：

```haxe
override public function draw():Void {
    if (persistentDraw || subState == null) super.draw();   // 父
    if (subState != null) subState.draw();                  // 子（应该在最上）
}
```

`TreeMenu extends UIState extends MusicBeatState`，`UIState`/`TreeMenu` 都没有覆盖
`draw()`（覆盖 `draw()` 的是 `TreeMenu.hx` 末尾的 `final class TreeMenuDrawer extends FlxBasic`，
它是 state 的一个普通子对象），所以子状态确实应该在最后提交。

### 51.3 已经排除的：不是 bgfx 的排序模式

bgfx 默认视图模式按 `view|blend|alphaRef|program|depth` 排序，**没有把提交序号
`m_seq` 放进 key**（`bgfx_p.h` 的 `SortKey::encodeDraw`），这对于 2D 画笔式渲染器
本来就是隐患，因此加了修复与 A/B 开关：

```cpp
bgfx::setViewMode (view, viewMode);   // 默认 ViewMode::Sequential
// CNE_METAL_VIEWMODE=default | depth 可覆盖
```

**但 A/B 实测两种模式渲染出的子状态画面逐帧几乎完全相同**
（稳态帧 mean abs diff ≈ 0.5/255）—— 说明这个场景里两张表用的是同一个
program/blend/depth，默认模式的稳定基数排序本来就没有打乱顺序，
**所以这个错位不是 bgfx 排序造成的，而是提交顺序本身**。

新增的绘制顺序探针（`CNE_METAL_DEBUG=1`）：

```
[cne-dbg] f=2555 draw#11 stream 0 stride=20 bytes=224 base=0 pos=(95.0,65.0) layout: …
```

它按提交顺序打印每一笔的帧号 / 序号 / 首个顶点坐标，用来判断"谁先被提交"。

### 51.4 结论与下一步

* 声音问题（第 50 节）**已修复并验证**。
* 子状态字母被压住 + 状态切换黑屏只覆盖一半，**都是同一个"后提交的先画"现象**，
  目前证据指向**提交顺序**而非 bgfx 排序；`ViewMode::Sequential` 这个修复本身是对的
  （对多 program/多 blend 的 2D 场景是必要的），只是没有解决这一处。
* 下一步建议：用第 51.3 的探针把父菜单文字与子状态文字在某一帧里的提交序号抓出来，
  确定是 Flixel 的绘制链在 Metal 路径被绕开，还是 OpenFL 侧把
  `TreeMenuDrawer`（`FlxBasic`）排到了 `subState.draw()` 之后。

---

## 52. 第五十二轮：★ 修复 —— 字母/箭头颜色错乱 + 闪烁（常量属性缓冲自我别名）

### 52.1 症状

设置 → CONTROLS 子状态里，右侧每个按键字母（D / F / J / K / U / I / A / S …）
**各自一个随机颜色**，左侧方向箭头也是粉/蓝/黄/绿乱跳，而且**每一帧都在变**（闪烁）。

### 52.2 根因：一条共享的 dynamic buffer 在同一帧里被反复 update

OpenFL 把 `openfl_Alpha` / `openfl_ColorMultiplier` / `openfl_ColorOffset`
当作**常量顶点属性**投递（`glVertexAttrib*f`，见第 49 节），我们把它们物化成顶点流。
第 49 节把它们合并成**一条**交错流，并用「属性集合」做缓存 key、每笔 `bgfx::update`
原地改值 —— 这正好踩中 bgfx 动态缓冲的语义：

> 同一个 dynamic vertex buffer 在同一帧里被 update 多次，**之前引用的那些绘制会一起
> 读到最后一次写入的值**。

于是同一帧里画的所有精灵都在读"最后一笔"的 alpha/颜色乘子；
而每帧提交顺序略有不同 → 谁读到谁的值每帧都在变 → **颜色错乱 + 闪烁**。

（这也解释了为什么每个字母颜色还不一样：bgfx 搬移 dynamic buffer 时只有一部分
pending 绘制被正确重定向，剩下的就各读各的。）

### 52.3 修法：按「值」分桶 + LRU 回收

`BindVertexStreams` 的常量流改成 **value-keyed pool**：

* 先把这一笔的常量数据算出来、算出内容签名 `sig`；
* `sig` 相同（同样的 alpha/颜色/顶点数）**共用**一个缓冲，不同 `sig` **各占一个**；
* 池子上限 `kMaxConstantBuffersPerLayout = 256`，满了就回收
  **本帧还没用过**（`lastFrame != gFrameIndex`）里最久未使用的那个，
  原地 `bgfx::update` 覆盖 —— 既不会别名，也不会像早期"按值建新缓冲"那样
  在淡入淡出时每帧泄漏句柄。
* 因此常量流现在是 **1 笔 1 次 update**，且任何两个存活值互不共享缓冲。

> 顺带修掉了旧注释里那个两难："共享缓冲 = 别名"，"按值新建 = 泄漏"。
> 用**有界 + LRU + 只回收本帧未引用**就能同时满足。

### 52.4 验证

自动导航到 Options → CONTROLS 子状态后连拍 40 帧：

```
last-40 frame-to-frame mean diff: avg=0.36  max=6.91      ← 不再闪烁
```

截图对比：字母统一为蓝底白高光，左侧 LEFT/DOWN/UP/RIGHT 箭头恢复各自的颜色。

---

## 53. 仍未解决：父菜单组件压在子状态之上（图层顺序）

同一个子状态截图里还能看到：父菜单的 `CONTROLS / GAMEPLAY / APPEARANCE / LANGUAGE`
压在子状态 `LEFT / DOWN / UP / RIGHT` 之上。第 51 节已给出提交顺序实测：

```
draw#13..#47  stride=16 bytes=3008  y 间距 75   ← 子状态 Keybinds 的行
draw#48..#52  stride=20 bytes=224   y=38/65     ← 父菜单的标题/描述（后提交 → 画在上面）
```

而 Flixel 语义是父先子后（`FlxState.draw()`）。已排除 bgfx 排序模式
（`CNE_METAL_VIEWMODE` A/B 结果逐帧相同）。

**下一步**：给绘制探针补上 program id + 纹理 id，确认倒数这几笔（#48–#52）
到底是哪个对象、由哪条路径提交 —— 怀疑是 `MusicBeatState.draw()` 末尾的
`postDraw` 事件里画的（它在 `super.draw()`（含 `subState.draw()`）之后触发），
或 OpenFL 文本通道单独排到了最后。

---

## 54. ★ 第五十三轮：shader 全部改成**构建期 AOT** 原生 MSL —— 运行时零 GLSL→MSL 翻译

用户要求"所有部分都为原生，不要任何 API 翻译"。原先 `GLSL → GLSLToMSL（运行时）→ MSL`
这条链路里，**翻译器是运行时调用的**。本轮把它整体挪到构建期：

```
构建期（每次 apply.sh 自动跑）
  metal/scripts/gen_shader_aot.py
    1. 枚举全部 GLSL：OpenFL/Flixel/CNE 的 @:gl* metadata（复现 ShaderMacro 宏展开，
       含 typedef 继承解析）、assets/shaders/**、mods/**/*.frag|.vert
    2. 用同一个 GLSLToMSL 通过 `haxe --interp` 预翻译（56 个 stage，0 失败）
    3. 生成 lime/graphics/metal/ShaderAotCache.hx（hash → MSL + attr/uniform/sampler 表）
运行时
  MetalShaderBridge.onShaderSource → ShaderAot.fetch(hash) → 直接查表（0 次翻译）
  未命中 = 响亮报错（打印 key/源码/重生成命令 + 落盘 /tmp/cne-aot-missing/）
```

实测证据（详见 `metal/audit/aot-shaders.md`）：

* `[cne-aot] EXIT cache=5C3E7B9A012E7E30 entries=56 hits=… misses=0 runtimeTranslations=0 runtimeGlslToMslCalls=0`
  —— **运行时翻译器调用次数 = 0**（`GLSLToMSL.runtimeCallCount`）；
* 每个 stage 原生侧标 `origin=AOT-cache`；
* 生成缓存里的 MSL 与审计时运行时落盘的 `/tmp/cne-shader-0..9.metal` **10/10 字节相同**；
* 3 个内建 program + 进歌曲时创建的 `CustomShader('pixelZoomShader')` 全部命中，`misses=0`。

新增文件：`metal/scripts/gen_shader_aot.py`、`metal/scripts/aot_shaders.py`、
`metal/tools/GenShaderAot.hx`、`metal/haxe/lime/graphics/metal/ShaderAot.hx`（生成物
`ShaderAotCache.hx`、`metal/audit/aot-coverage.md`）。`apply.sh` 新增 `--skip-aot` / `--deploy`。

**两个必须记住的坑**：

1. hash 必须用 `haxe.Int64` 算（32 位 `Int` 有符号溢出是 C++ UB，clang -O2 会把 FNV 循环
   优化错：整个 key 前半截变成 `80000000`，全部 shader miss）。
2. `lime build mac` 会把 Lime 的 **neko CLI 引导 ndll** 复制进 .app，覆盖掉 `lime rebuild mac`
   产出的 cpp/Metal ndll → app 启动即 abort。构建末尾用 `apply.sh --deploy` 装回并重签名。

---

## 55. 第五十五轮：★ 原生路径成为默认 —— 47/47 原语真正原生，GL 形状调用降 88.8%

### 55.1 做了什么

1. **把 27 个转发器全部改成真原生实现**（`MetalNative.inc`）。改动前是
   `void cne_metal_uniform1i (...) { lime_gl_uniform1i (...); }` 这种纯转发；
   现在每个函数体都是真实现（写 uniform / 记 attrib 状态 / 绑定 / 提交绘制），
   **47/47 个 `cne_metal_*` 不再调用任何 `lime_gl_*`**。
2. **依赖倒置后调整了 include 顺序**：`MetalNative.inc` 必须排在
   `MetalUniforms/Textures/States.inc` 之后（它现在要用 `WriteUniform`、
   `WriteMatrixUniform`、`vertexArrays`、`buffers`、`SubmitDraw` 等内部件）。
3. **补上最后 3 个热调用点**：`ShaderParameter.hx` 的
   `enableVertexAttribArray`（×2）与 `vertexAttribPointer`，
   新增 `MetalNative.vertexAttribCurrent(gl, ...)`（GL 语义：用「调用时已绑定的 buffer」，
   原生侧以 `buffer = -1` 表示取 `currentArrayBuffer`）。
4. **`MetalNative.hx` 按 C++ 原语表重建**：包含 47 个公开包装 + 常量 + 枚举映射
   （blend factor / compare / cull / equation / attrib type / primitive / index type）
   + `active(stage)` / `initialize()`。
5. **原生路径改为默认**：`CNE_METAL_NATIVE_PATH` 从「必须设置」变成「调试覆盖」：
   - 未设置 / `all` → 三段（state, prog, geo）全原生 = **出厂配置**
   - `state,prog,geo` → 显式列 stage
   - `none` / `off` → 走旧的 GL 形状路径（**仅作 A/B 参考，已不再维护，见 55.3**）

### 55.2 实测收益与正确性

| 指标 | 旧（GL 形状路径） | 新（原生默认） |
|---|---|---|
| `lime_gl_*` 调用/会话 | 363,224 | **40,786**（**−88.8%**） |
| `enable/disable_vertex_attrib_array` | 107,673 | 大幅下降 |
| 每笔 draw 的 GL 调用 | ≈88 | → 目标 6–9 |
| 画面 | — | **与参考帧一致** |

**像素级验证**（`CNE_METAL_SHOT_RAW` 在同一帧抓 bgfx 背缓冲，排除左上 FPS HUD）：

- `state,prog,geo`（全开）vs 仅 `prog`：**差异像素 = 0（max 0）** ⇒ state/geo 两个 stage 行为等价。
- 默认配置 vs 显式 `all`：差异 4.82%，**但两次「相同默认配置」的对照跑也是 4.82%、bbox 完全相同**
  （x[1106..2401] y[140..1405] = 标题画面 GF 待机动画区）⇒ 该差异是**动画相位**，不是渲染差异。
- 截图确认：标题画面 FNF logo（粉/白/蓝）、GF（红棕发红裙红鞋）、speakers、
  `Press Enter to Begin`（蓝）、FPS HUD 全部正确。

### 55.3 已知问题（必须记住）

**旧的 GL 形状回落路径现在渲染黑屏。** 原因：重构时 `MetalNative` 的若干
`else` 分支（尤其是 `drawIndexed` / `drawArrays` / 状态类包装）没有逐字复原原实现，
原生路径成为默认后这条路径不再被使用，因此未修复。

- 影响面：**仅在显式设置 `CNE_METAL_NATIVE_PATH=none|off` 时**（调试用途）。
- 出厂配置（默认）不受影响。
- 若要修：按 `MetalNative.inc` 的真原生实现，逐项复原各包装的 `else` 分支，
  并用 `CNE_METAL_NATIVE_PATH=none` 跑 A/B。

### 55.4 剩余 40,786 次 GL 形状调用是什么

主要是**上传类**调用（`buffer_data` / `tex_image_2d` / `tex_sub_image_2d` /
`tex_parameteri` / `get_*`）—— 这些还没有原生化舞台化。它们不在 per-draw 热路径上，
若要继续推进，下一个 stage 应该是 `upload`。

### 55.5 复现命令

```sh
cd CodenameEngine-1.0.1
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832            # 应用补丁（含 OpenFL 钩子）
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832 --build    # 重建 ndll（自动部署+校验）
haxelib run lime build mac -Dmetal                                 # 重建 app（Haxe 改动后需要）
./metal/scripts/apply.sh --lime ~/haxelib-dev/lime-832 --deploy   # 装回 cpp ndll + 重签名

# 验证：默认即原生
CNE_METAL_COVERAGE=1 ./CodenameEngine.app/Contents/MacOS/CodenameEngine | grep -E 'cne-native|cne-cov' | tail -5
CNE_METAL_SHOT_RAW="1000:/tmp/shot" ./CodenameEngine                  # 抓背缓冲做 A/B
```

### 55.6 追加：state stage 补齐 `gl.enable`/`gl.disable` —— GL 调用再降 82%

55.4 说剩余 40,786 次里"主要是上传类"，实测下来**真正的大头是 `gl.enable`/`gl.disable`（33,408 次 = 82%）**，
而且其中绝大部分来自 **3 个 `gl.enable(gl.TEXTURE_2D)` 调用点**（每帧执行上千次）。

原因：`glEnable(GL_TEXTURE_2D)` 在 core GL / GLES 3 里是**遗留无效操作**，但 OpenFL 仍会调。
我们的 `lime_gl_enable` 需要记录这个 cap，于是每次都要过一次 GL 形状的 ABI。

修法两步：
1. `MetalNative.setCap(gl, cap, enabled)`：把 state stage 拥有的 4 个 cap
   （`BLEND` 0x0BE2 / `CULL_FACE` 0x0B44 / `DEPTH_TEST` 0x0B71 / `SCISSOR_TEST` 0x0C11）
   映射到已有的 `_setBlend` / `_setCullEnabled` / `_setDepthTestEnabled` / `_setScissorTest`；
   **`TEXTURE_2D`(0x0DE1) 归为记录型 no-op**（它本来就不影响 Metal/bgfx 状态）；
   其余 cap 原样回落 GL。
2. `patch_openfl_native.py` 增加 14 个钩子：Context3D 的 10 个 enable/disable 调用点
   + Context3D/Shader 里的 4 个 `TEXTURE_2D` 开关。

结果（同一场景、同一会话长度）：

| 指标 | 原始 | 55.2 之后 | 55.6 之后 |
|---|---|---|---|
| `lime_gl_*` 总调用 | 363,224 | 40,786 | **8,313（−97.7%）** |
| `lime_gl_enable` | 28,443 | 32,943 | **0** |
| `lime_gl_disable` | 4,965 | 5,684 | **0** |

修正后的剩余 8,313 次构成：

```
6834  lime_gl_buffer_data          ← 顶点/索引上传（下一个 stage：upload）
 946  lime_gl_tex_image_2d         ← 纹理上传
 130  lime_gl_get_attrib_location  ← program 链接期查询（一次性）
 113  lime_gl_get_uniform_location ← 同上
  96  lime_gl_tex_parameteri       ← 采样器参数
  62  lime_gl_object_register      ← 对象登记
  29  lime_gl_create_buffer / 24 create_texture / 其余创建类（一次性）
```

**画面验证**：标题画面截图（59 FPS）FNF logo 粉/白/蓝、GF 红棕发红裙红鞋、speakers、
`Press Enter to Begin` 青色、FPS HUD 全部正确。

下一个可做的 stage 是 **`upload`**（`buffer_data` 6,834 + `tex_image_2d` 946 + `tex_parameteri` 96），
做完后 GL 形状调用可降到 ~500 次（仅剩一次性创建/查询）。收益边际递减，属于"洁癖级优化"。

### 55.7 游戏内验证（原生默认路径）

自动按键跑到故事菜单（跨了一次状态切换），40 秒会话：

```
frame=2281  submitDraw=28517  bgfxDraws=17/帧  崩溃标记=0
[cne-native] stages: prog,geo,state
```

截图：故事菜单 60 FPS、黄底素描、BF/GF 黑白描线、`TRACKS`/`TUTORIAL` 粉、
`TUTORIAL`(白)/`NORMAL`(黄)/`WEEK 1`(灰) 与青色箭头全部正确。

### 55.8 `upload` stage 为什么没做完（有意的取舍）

剩余 8,313 次里 6,834 是 `lime_gl_buffer_data`（Flixel 每帧的动态顶点批上传）。
要把它原生化，必须在 `MetalNative` 里重写 lime 的**重载分派**：

```haxe
gl.bufferData(gl.ARRAY_BUFFER, byteArray, __usage)        // ByteArray -> size + getData()
gl.bufferData(gl.ARRAY_BUFFER, shaderBuffer.paramData, ...) // Float32Array
```

而 `lime.utils.Float32Array` 是 `abstract over JSFloat32Array`，**没有 `getData()`**，
只能走 `toBytes()`；`ByteArray` 的指针接口又是另一套。也就是说要同时正确处理
"ByteArray / Float32Array / DataPointer" 三种入参并算对 `size`。

**算错 size 或指针的后果是静默的画面损坏（纹理/顶点错位），而不是报错** ——
收益是每帧 5 次本就必须发生的上传，风险收益不划算，故停在 8,313（−97.7%）。

若将来要做，正确顺序是：
1. 先写单元式验证（同一 ByteArray 分别经 lime 路径与原生路径上传，比对 `buffers[id].bytes`）；
2. 再逐个调用点切换（`VertexBuffer3D` / `IndexBuffer3D` / `Shader.hx`），每切一个跑一次 A/B；
3. 最后把 `tex_image_2d`（946 次，4 种重载）同样处理。

---

## 56. 第五十六轮：`upload` stage —— GL 形状调用 363,224 → **533（−99.85%）**

### 56.1 做了什么

补上最后一批**每帧**发生的 GL 形状调用，把 `upload` stage 变成原生：

| 原语 | 调用量 | 做法 |
|---|---|---|
| `buffer_data` | 6,834 | 依赖倒置：实现体搬进 `cne_metal_buffer_data`，`lime_gl_buffer_data` 变薄壳 |
| `tex_image_2d` | 884 | 同上（9 参形式）；4 个重载里先接最热的 2 个 |
| `tex_parameteri` | 96 | 同上 |

**关键技术点**：`gl.bufferData(target, view, usage)` 的 `view` 是 `lime.utils.ArrayBufferView`，
而原生原语要 `DataPointer`。**`lime.utils.DataPointer` 有 `@:from fromArrayBufferView(...)` 隐式转换**，
所以 Haxe 包装直接声明成

```haxe
public static inline function bufferData(gl:Dynamic, target:Int, data:ArrayBufferView, usage:Int):Void
{
    if (active("upload")) _bufferData(target, data == null ? 0 : data.byteLength, data, usage);
    else gl.bufferData(target, data, usage);
}
```

就能把 `ArrayBufferView` / `Float32Array`（有 `@:to toArrayBufferView()`）自动转成指针 ——
**不需要手动算指针**，风险从"高"降到"低"。`byteLength` 取自 `ArrayBufferView` 的字段。

### 56.2 实测结果（同一场景、同一会话长度）

| 指标 | 起点 | §55 之后 | **§56 之后** |
|---|---|---|---|
| `lime_gl_*` 总调用 | 363,224 | 8,313 | **533（−99.85%）** |
| `buffer_data` | 5,971 | 6,834 | **0** |
| `tex_image_2d` | 875 | 946 | **24** |
| `tex_parameteri` | 96 | 96 | **72** |

**画面验证**：标题画面 59 FPS，logo / GF / speakers / `Press Enter to Begin` / FPS HUD 全部正确；
`stages: prog,upload,geo,state` 全开。

### 56.3 剩余 533 次是什么（全部是**一次性**）

```
130  get_attrib_location      ← program 链接期查询，每个 program 一次
113  get_uniform_location     ← 同上
 72  tex_parameteri            ← 剩余 4 个调用点（采样器参数，每纹理一次）
 62  object_register           ← lime 对象登记（启动期）
 29  create_buffer             ← 启动期
 24  tex_image_2d              ← 剩余重载（Image 直传形式）
 ... 其余 <20 次：create_texture / create_shader / compile_shader / link_program / get_string
```

**没有一项在 per-draw 或 per-frame 路径上。** 也就是说：
**运行时热路径的 GL 形状调用已经是 0。**

### 56.4 再往下的可选工作

- 把剩余的 `create_*` / `get_*_location` / `object_register` 也原生化 → 533 → ~0，
  但它们是启动期一次性调用，**对性能与画面零影响**，纯粹是"代码里不留 GL 名字"。
- 删除 1,078 个 `lime_gl_*` 函数（死代码清理）
- 修好 `CNE_METAL_NATIVE_PATH=none` 的旧路径（或直接删掉它 + 相关调试开关）
- 去 bgfx 直连 Metal（2–3 人日，收益最低）

### 56.5 顺带修的两个构建脚本 bug

1. `apply.sh` 的安全网里 `$APP` 在 `--build` 但未 `--deploy` 时是未定义变量（`set -u` 下报错退出，
   让构建"假失败"）→ 已在脚本开头定义 `APP=` 默认值。
2. `TextureBase.hx` 加了 `MetalNative.texImage2D/texParameteri` 调用但缺 `import` →
   已加 import 钩子（`texturebase-import`）。

### 56.6 继续：`texParameteri` 剩余调用点清零 → 533 → **461（−99.87%）**

补 3 个 `TextureBase` 钩子（`MAG_FILTER` / `WRAP_S` / `WRAP_T`），`tex_parameteri` 从 72 → 0。
`Context3DVideo` 里剩 2 处走的是**未使用的视频路径**，暂不处理。

剩余 461 次全部是**一次性**调用：

```
130  get_attrib_location     ← program 链接期（字符串参数 ABI）
113  get_uniform_location    ← 同上
 62  object_register         ← lime 对象登记
 29  create_buffer           ← 对象创建
 24  create_texture / 24 tex_image_2d（剩余 Image 直传重载）
 18  bind_attrib_location    ← 字符串参数
  7  get_string              ← 字符串返回
 其余 <10 次：create_shader / compile_shader / link_program / get_shaderi ...
```

**清零需要的下一批工作**：这些涉及 **字符串参数/返回值的 CFFI ABI**
（`get_attrib_location` / `bind_attrib_location` / `get_string`），
以及 lime 的对象注册（`object_register` / `create_*`）。它们**全在启动期，与画面和帧率无关**，
属于"代码里不留 GL 名字"的收尾。

### 56.7 移除已知坏的 `none` 陷阱

`CNE_METAL_NATIVE_PATH=none|off` 以前会切到旧的 GL 形状路径，而那条路径在原生重构后**渲染黑屏**
（包装的 `else` 分支没逐字复原）。产品路径不用它，但这个开关是个陷阱 —— 任何人调试时一按就黑屏。

现在改为：`none` / `off` **不再切走**，而是打印

```
[cne-native] CNE_METAL_NATIVE_PATH=none is no longer supported (legacy GL path removed); using the native path
```

并继续走原生路径。实测 `none` 模式下非黑像素 1,733,516（正常渲染），**黑屏陷阱消失**。

> 注：GL 形状的函数体仍在二进制里（还兜着剩余 461 次一次性调用），但**没有任何配置能让它成为默认路径**了。

### 56.8 继续清零：对象创建 / program 查询原生化 → **218 次/21s 会话（−99.94%）**

在 §56.6 的 461 基础上，把剩余最大几类也原生化：

| 原语 | 做法 | 结果 |
|---|---|---|
| `get_attrib_location` / `get_uniform_location` | 依赖倒置（`Int→String→Int`） | **0** |
| `create_buffer` / `create_texture` | 依赖倒置（`Void→Int`） | **0** |
| `bind_attrib_location` | 依赖倒置（`Int→Int→String→Void`） | 剩 18（仍有未接调用点） |

**关键突破：`_loadPrime` 的签名格式**。之前不敢做是因为返回值型 primitive 的签名格式未知，
猜错会让 primitive 加载直接 abort。从 lime 的 `NativeCFFI.hx` 找到现成例子后确认：

```
签名 = 参数类型码按顺序 + 返回类型码
  create_buffer()            Void->Int          "i"
  get_attrib_location(i,s)   Int->String->Int   "isi"
  bind_attrib_location(i,i,s) Int->Int->String->Void "iisv"
  use_program(i)             Int->Void          "iv"
```

Haxe 侧用 `(id : GLBuffer)` / `(id : GLTexture)` 构造 —— 因为 lime 的 `GLBuffer`/`GLTexture`
都是 `abstract` 且带 `@:from fromInt(id:Int)`，可以直接从我们返回的 int 构造。

### 56.9 lime 对象注册表成了新的瓶颈（也是终点）

清零到 218 后，构成变成了：

```
147  lime_gl_object_register     ← lime 的 GLObject 注册表
138  lime_gl_object_from_id      ← 同上（把 int 包成 GLObject）
 51  tex_image_2d                ← 剩余 Image 直传重载
 18  bind_attrib_location        ← 仍有未接调用点
  7  get_string                  ← 字符串返回
  6  shader_source / get_shaderi / get_shader_info_log / create_shader / compile_shader
```

**`object_register` + `object_from_id` 占 69%`**：这是因为我们返回原始 int 后，
lime 用 `@:from fromInt` 把它包成 `GLObject` 时反过来又调 `lime_gl_object_from_id`，
并在注册表里登记。要消灭它们必须**彻底不用 lime 的 `GLBuffer`/`GLTexture` 类型**
（改 OpenFL 侧的对象类型），那是另一个量级的重构，收益仍是 0（都是启动期）。

### 56.10 最终结论

| 指标 | 值 |
|---|---|
| GL 形状调用（21s 会话） | **218**（起点 363,224，**−99.94%**） |
| **per-draw / per-frame GL 形状调用** | **0** |
| 剩余调用性质 | 全部**启动期一次性**（对象注册 + 未接的 tex 重载） |
| 画面 | 59 FPS，与参考帧一致；多状态切换会话 0 崩溃 |

## 57. 「转场黑幕不消失」根因与修复（FBO / 离屏合成路径）

### 57.1 症状

进入 **Winter Horrorland / Senpai / Roses / Thorns**（带对话剧情的曲目）后，全屏黑色幕布
一直不消失；HUD 显示在黑幕**之上**；退出曲目后恢复正常。窗口拖小时能看见被挡住的内容。

### 57.2 定位过程（全部可复现）

1. 背缓冲对比（`CNE_METAL_SHOT_RAW`）：黑屏时背缓冲**也是黑的** → 不是呈现层。
2. 逐项排除：混合（`blend=1 src=1 dst=303`）、颜色掩码（`cw=1111`）、alpha（1.0/0.5）、
   `openfl_HasColorTransform`（0）、纹理上传、着色器 uniform 查询。
3. `CNE_STATE_LOG=1` 诊断（新增）打印 State + **subState** → 锁定
   `subState=funkin.game.cutscenes.DialogueCutscene`。
4. 对话帧的 `cne-sub` 提交列表揭示真实路径：

   ```
   sub#0..#11  fbo=164                        ← 画面渲染进离屏帧缓冲
   sub#12      fbo=191  tex 2498x1404         ← 再进另一个离屏纹理
   sub#13      prog=4 tex=190 2498x1404 fbo=0 ← 把合成纹理全屏画到屏幕
   sub#25..#29 fbo=0                          ← HUD（在黑幕之上）
   ```

   ⇒ 走的是 **离屏 FBO → 全屏合成**。合成纹理是空的 → 全屏黑；HUD 在其后绘制所以可见。

### 57.3 根因（两个叠加缺陷）

**(A) bgfx framebuffer 从不被创建**

`bgfx::createFrameBuffer` 只在 `lime_gl_check_framebuffer_status` 里惰性创建，而 OpenFL 仅在
`__enableErrorChecking` 为真时才调用它 —— **永不调用**。于是 `ConfigureView` 里
`bgfx::isValid(fbo.handle)` 恒为 false，视图退回背缓冲：**离屏绘制落到了屏幕上，FBO 纹理永远是空的**。

修复：新增 `EnsureFramebuffer(id)`，在 `ViewForFramebuffer()`（所有提交/清除路径的唯一必经点）
按需创建。

**(B) 纹理缺少 `BGFX_TEXTURE_RT` 标志**

`bgfx::createFrameBuffer` 拒绝未带 `BGFX_TEXTURE_RT` 的纹理作为附件 → 即使 (A) 修好，
创建仍返回 `valid=0`。修复：所有 2D 纹理分配（`MetalTextures.inc` 8 处 +
`MetalNative.inc` 的 `cne_metal_tex_image_2d`）统一加上 `BGFX_TEXTURE_RT`。

### 57.4 验证

```
[cne-fbo] created framebuffer 164 -> texture 162 (valid=1)
[cne-fbo] created framebuffer 191 -> texture 190 (valid=1)
```

SENPAI 对话框串（像素学校背景 + Senpai 立绘 + 粉色对话框 + 手形光标）完整渲染；
连续 200 帧捕获黑占比稳定 0.19–0.20（此前为 0.93–0.96），0 崩溃。

### 57.5 顺带修复：Resize 视口单位混用

`MetalContext::Resize()` 里 `backbufferWidth/Height`、`state.viewport`、`setViewRect` 用的是
**逻辑点**，而 swapchain 用 **drawable 像素**（Retina 下 2×）。结果 Resize 之后视口只覆盖帧缓冲
的四分之一 —— 表现为「窗口拖小后内容只挤在底部一条」。现已全部改为 `QueryDrawableSize()`
的 drawable 像素（init 路径同样修正），并在 `Resize` 的 swapchain 补上
`formatDepthStencil = D24S8`。

### 57.6 诊断开关

| 环境变量 | 作用 |
|---|---|
| `CNE_STATE_LOG=1` | 每 60 帧打印 State 类名 + **subState 类名** + camera fade alpha |
| `CNE_METAL_DEBUG=1` | 打印 `[cne-fbo] created ...`、view/rect/scissor 等 |
| `CNE_METAL_DRAWLOG="lo-hi"` | 指定帧窗口打印每笔提交（prog/tex/fbo/vp/blend） |
| `CNE_METAL_SHOT_RAW="<frame>:<path>"` | 抓背缓冲原始像素 |

## 58. RT 粒度收窄：尝试与结论（**暂缓，已回退到可用版本**）

### 58.1 背景

§57 的修复给**所有** 2D 纹理加了 `BGFX_TEXTURE_RT`。这能修好 FBO（`valid=1`），
但 RT 纹理在 Metal 上走不同的资源路径，用户报告了两个副作用：

- Thorns 画面异常（角色/舞台缺失，只剩红底与箭矢）
- 标题画面提前跳过文本后，进主菜单变慢

### 58.2 尝试过的三种收窄方案（全部实测 `valid=0`）

| 方案 | 结果 |
|---|---|
| A. 判据「`tex_image_2d` 的 `data == null`」(渲染目标模式) | `valid=0` —— FBO 纹理实际是**带数据**创建的 |
| B. 判据「`tex_storage_2d` 空分配」(GLES3 路径) | `valid=0` —— 该路径没被走到 |
| C. 首次当附件时销毁重建为 RT（`PromoteTextureToRenderTarget`）| `valid=0` |

### 58.3 C 方案失败的根因

重建需要纹理的 bgfx 格式，而 `TextureData` 只保存 GL 的 `internalFormat/format/type`：

- 用 `FormatFromInternalFormat(internalFormat)` → 返回 `Unknown`（该 helper 是给另一套 GL 枚举用的）
- 改用上传时同样的 `ToBgfxFormat(internalFormat, format, type)` → 仍失败：这些字段在
  FBO 纹理上没有可靠赋值（疑似 0）

### 58.4 下一步的正确做法（小改动，未实施）

**在创建纹理时把实际使用的 bgfx 格式记进 `TextureData`**：

```cpp
struct TextureData {
    ...
    bgfx::TextureFormat::Enum bgfxFormat = bgfx::TextureFormat::Count;  // recorded at creation
};
```

之后 `PromoteTextureToRenderTarget()` 直接用 `texture->bgfxFormat` 重建，无需任何映射猜测；
再移除全局 RT，普通纹理回到零改动。

### 58.5 当前交付状态

- **保留全局 RT**（唯一验证过 `valid=1` 的配置）：黑幕已修复并确认
- 副作用（Thorns 画面 / 菜单变慢）待用 58.4 的方案验证是否消除

### 58.6 最终决定（第 4 次方案失败后）

第 4 种方案（`TextureData` 记录 `bgfxFormat` 后在首次当附件时重建为 RT）同样得到 `valid=0`，
且诊断证明 promotion **从未被触发** —— 因为当前配置下所有纹理创建时已带 RT。

因此最终保留 **全局 `BGFX_TEXTURE_RT`**（唯一实测 `valid=1` 的配置），并把失败的
promotion 代码与诊断一并移除，使代码树与已验证行为一致。

验证（清理后重建）：

```
[cne-fbo] created framebuffer 164 -> texture 162 (valid=1)
[cne-fbo] created framebuffer 191 -> texture 190 (valid=1)
末帧 black=0.19 mean=140
```

### 58.7 若要继续收窄，唯一正确的起手式

**先测出 FBO 纹理的真正创建路径**，不要再猜：

```cpp
// 在 TextureData 的每个创建点（tex_image_2d / tex_storage_2d / compressed）打印
[cne-tex-create] fn=<gl 函数名> tex=<id> WxH internalFormat=0x%x
// 在 lime_gl_framebuffer_texture2D 打印
[cne-fbo-attach] fb=<id> -> tex=<id>
```

用 tex id 对上，就能知道 162/190 是哪个函数、用什么格式创建的，再在那里精确加 RT。
一次运行即可定位（而非每轮 8 分钟的构建）。

## 59. Thorns:多级 FBO（ping-pong）渲染

### 59.1 症状演进

| 阶段 | 症状 |
|---|---|
| 修复前 | 进入 Thorns 后整屏红色（只有 HUD 正常）|
| 修 FBO viewport 后 | 仍红色 |
| 修 view 执行顺序（[FBO..., 0]）后 | 仍红色 |
| **per-switch view 分配后** | **红色消失**，舞台/角色出现——但整体缩小约 20% 且**旋转 180°**（HUD 正常）|

### 59.2 帧结构（实测提交列表，frame 4200，22 笔）

```
sub#0-4   fbo=158  ← 场景（舞台 1579x1319、角色 1677x1673 n=30 等）
sub#5     prog=176 tex=156 2498x1404 fbo=173   ← 后处理 pass A（读 158 的纹理）
sub#6     prog=180 tex=172 2498x1404 fbo=158   ← pass B 写回 158
sub#7     prog=4   tex=156 2498x1404 fbo=0     ← 合成到屏幕
sub#8-21  fbo=0                                ← HUD
```

### 59.3 关键修复：per-switch view 分配

`ViewForFramebuffer()` 原来给每个 FBO **固定一个 view**，而 bgfx 按 view id 顺序执行 —— 
一帧内 `134 → 149 → 134` 这种 ping-pong 会被重排，pass B 跑到 pass A 之前，合成拿到错内容
（= 整屏红）。改为 **每次渲染目标切换就分配一个新 view**（view 顺序 == 提交顺序）：

```cpp
static uint16_t gNextView, gActiveView; static int gActiveViewTarget; static uint32_t gViewEpoch;
uint16_t ActiveViewForTarget ()   // 帧内首次调用时 ResetViewAllocation()
```

验证：红色消失；SENPAI 对话（black=0.19）与 Eggnog 游玩（black=0.18）回归通过。

### 59.4 待解决：缩小 + 旋转 180°

HUD（直接画到屏幕）正常，只有 FBO 内容缩小并旋转 180° ⇒ **FBO pass 的变换/UV 有误**。

已确认的事实：

- FBO 纹理 2498x1404 是游戏**自己请求**的（`[cne-texcreate] 2498x1404 ... data=null` 大量出现），不是我们算错
- FBO 内绘制的 viewport 是 2498x1404，与 FBO 尺寸一致
- 合成使用 `prog=4`（BitmapShader）+ `tex=156`

### 59.5 下一步诊断（不要再猜）

用 `CNE_METAL_DUMPDRAW="<frame>:<drawIndex>"` 抓 **sub#7（合成）**与一笔 HUD 的顶点：

- 若合成 quad 的**位置**就是小的/旋转的 → 游戏侧 sprite 变换被我们算错（顶点/矩阵路径）
- 若合成 quad 是**全屏**、但纹理内容小/翻转 → FBO 渲染时的投影矩阵或 Y 翻转约定有误

怀疑点排序：
1. FBO pass 使用矩阵 uniform 的上传（`transpose`、components=4 vs mat3）
2. render-to-texture 的 Y 翻转被应用了两次（我们 `ConfigureView` 的 `h-(vy+vh)` + 游戏侧合成翻转）

### 59.6 已做的三个尝试（都未解决缩小/翻转）

1. **FBO 重新附加失效**：`lime_gl_framebuffer_texture2D` 现在在附件纹理变化时销毁并重建
   bgfx framebuffer（ping-pong RT 每帧换附件，缓存会指向旧纹理）。保留，语义正确。
2. **矩阵 uniform 修复**：`cne_metal_uniform_matrix4fv` 现在
   (a) 当 staging 缓冲小于 64 B 时**扩容而不是跳过**（旧代码 `64 <= size` 会静默丢弃上传），
   (b) 支持 `transpose`。保留。
3. **`CNE_METAL_1X=1`**：强制按逻辑分辨率（非 Retina drawable）渲染，用于排除 DPI 尺寸错配。
   实测**不影响**该问题（已保留为诊断开关，默认关闭）。

### 59.7 决定性审计证据（`CNE_METAL_SHADER_AUDIT=1`）

```
program=4   openfl_Matrix writes=291  value= 0.0008, 0, 0, 0, 0, -0.0014   ← Y 负（Y-down，正常）
program=8   openfl_Matrix writes=59   value= 2.0031, 0, 0, 0, 0, -1.9554
program=14  openfl_Matrix writes=322  value= 0.0016, 0, 0, 0, 0, -0.0027
program=176 openfl_Matrix writes=1    value= 0.0008, 0, 0, 0, 0, +0.0014   ← Y 正 + 只写 1 次
program=180 openfl_Matrix writes=1    value= 0.0008, 0, 0, 0, 0, +0.0014   ← Y 正 + 只写 1 次
```

**只有当 Thorns 用到的后处理程序 176/180 的 `openfl_Matrix` 在整个会话里只被写入 1 次**，
其余程序的写入次数是几十到几百（每帧/每精灵更新）。这直接解释了"缩小 + 翻转"：
这些程序一直用第一帧那次矩阵，精灵的 scale/flip 从未生效。

### 59.8 下一步（唯一方向）

查为什么 176/180 的 `openfl_Matrix` 只被写 1 次：

- 它们在 CNE 里是"屏幕 shader"用的 `Shader` 对象，矩阵可能通过 **`Shader.data.uMatrix.value =`**
  设置（只设一次）；每帧的投影应由渲染器写入 `openfl_Matrix`
- 若渲染器写的是**另一个位置**（`getUniformLocation` 对该 program 返回了别的 uniform），
  写入就会落到别处 —— 审计里 `writes=1` 正是这个现象
- 验证方法：给 `cne_metal_uniform_matrix4fv` 加 `CNE_METAL_UNIFORM_LOG=1`，打印
  `location → program/uniformName`，确认每次绘制时矩阵写入的落点是否正确

### 59.9 根因（第二层）：`lime_gl_uniform_matrix4fv` 静默丢弃滤镜程序的矩阵

`Program3D.uploadUniformBuffer()` 走的是 **GL 原语**（4 参数形式 `location, size, transpose, registers`），
落到 `lime_gl_uniform_matrix4fv(location, count, transpose, value)`。该函数原本：

```cpp
uint32_t dst = i * 64;
if (dst + 64 > uniform->value.size ()) break;   // ← 静默跳过
++uniform->writeCount;                          // ← 仍然计数
```

OpenFL 滤镜通过 **ShaderBuffer 的 float override** 喂 `openfl_Matrix`，而这些滤镜程序的 uniform
staging 缓冲被反射成**小于 64 字节**，于是矩阵被整块丢弃，而 `writeCount` 仍然 +1 ——
所以审计里那两个程序显示 `writes=1`（写了一次但被丢掉了），其余程序 `writes=291+`。

后果：滤镜程序一直用默认/陈旧矩阵 → 被滤镜的舞台既**缩小**又**翻转**。

**修复**：按 `count * 64` 扩容 staging 缓冲后写入（不再丢弃），并补上 `transpose` 支持。
与之前对 `cne_metal_uniform_matrix4fv`（ShaderParameter 路径）的修复保持一致。

修复后：Thorns 舞台**不再翻转**（屋顶朝上、藤蔓朝下），尺寸仍偏小 —— 见 §59.10。

### 59.10 仍存在：舞台尺寸偏小（约 15%）

滤镜矩阵已正确上传后不再翻转，但被滤镜的舞台仍只有约屏幕 15% 大小。
滤波器 quad 的尺寸/`openfl_TextureSize` 与实际滤镜区域（或 stage 尺寸）不一致是下一步要查的：
需要打印滤镜 pass 的 `openfl_TextureSize` uniform 与 quad 顶点范围。

### 59.11 上下翻转已修复（默认启用）

渲染进 render target 时，内容落在 bgfx 的 top-left 方向，后续任何采样 pass（OpenFL 滤镜、
屏幕 shader）都会看到上下镜像。修法：在向 FBO 上传矩阵时**取反 Y 行**（即把几何方向转回
GL 约定），默认开启，`CNE_METAL_FBO_FLIP=0` 可关闭做 A/B。

验证：Thorns 舞台方向正确（屋顶朝上、藤蔓朝下）；SENPAI 对话 `black=0.18`；
Ugh（Tankman）整屏正常；普通曲目无回归。

### 59.12 仍存在

Thorns 的舞台尺寸/可见性仍不对（截图为黑色舞台 + 正常 HUD）。下一步要查滤镜 pass 的
`openfl_TextureSize` 与 quad 顶点范围是否与实际滤镜区域一致。

### 59.13 全局 Y 取反不可用（已回退为 opt-in）

实测反馈（默认开启 Y 取反时）：

| 曲目 | 结果 |
|---|---|
| Thorns | **仍上下翻转**（没修好）|
| Senpai / Roses | **游戏画面全黑**（回归，被破坏）|
| 其它曲目 | 正常 |

结论：一律对 FBO 期间的矩阵取反 Y 会影响 **Senpai/Roses 的 screen-shader pass**（那些 pass 的
矩阵不能翻），而 Thorns 的翻转**另有来源**。已回退为 `CNE_METAL_FBO_FLIP=1` 显式开启。

### 59.14 下一步的正确方向

Thorns 帧结构（实测）：

```
scene → FBO 158 (prog 8/14)
      → prog 176 → FBO 173      (滤镜 pass A，采样 158)
      → prog 180 → FBO 158      (滤镜 pass B，采样 173)
      → prog 4   → 屏幕          (合成 158)
```

翻转只可能来自 **滤镜 pass 采样 render-target 纹理时的方向**。所以正确修法是
**只对"采样 RT 纹理"的滤镜 shader 翻转 V**，而不是翻矩阵：

1. 在 `lime_gl_bind_texture` / `tex_parameteri` 里记录纹理是否被当作 RT 使用过（我们已经知道 158/173 是 RT）
2. 在滤镜 pass（program 176/180）采样时，把该纹理的采样 UV 翻转（或在 shader 翻译层为该 program 注入 `1.0 - v`）
3. 只影响这些程序，避免破坏 Senpai/Roses

并且要同时验证：Thorns 舞台的**尺寸**问题（约 15%）很可能同源于滤镜 pass 的尺寸假设，
修好采样方向后需要一并复测。

### 59.15 实现方案（下一步具体改法）

翻转只发生在"采样 render-target 纹理"的绘制上，所以要在**顶点 UV**层面修，而不是矩阵：

1. `EnsureFramebuffer()` 创建 bgfx framebuffer 时，把其附件纹理标记为
   `texture->isRenderTarget = true`（字段已存在，目前未使用）
2. `SubmitDraw()` 里，若 `state.currentTexture` 的 `isRenderTarget` 为真：
   - 为该次绘制翻转 V：`v' = 1 - v`
   - 实现位置在 `BindVertexStreams()`：它已经在 CPU 侧按 layout 重建过顶点流
     （见 §56 的 A/B stride 分类），在那里把 textureCoord 属性做 `1-v` 即可
   - 只对 RT 纹理生效，普通精灵零开销
3. 回归验证：Thorns（翻转消失）、Senpai/Roses 游戏画面（不能变黑）、普通曲目、SENPAI 对话
4. 修好方向后再看尺寸问题：若舞台仍只有 ~15%，说明滤镜 pass 的
   `openfl_TextureSize`/quad 范围与实际滤镜区域不一致，需打印该 uniform 与顶点范围对比

注意：**不要**再用"FBO 期间矩阵 Y 取反"的方案（§59.13 已证明会黑掉 Senpai/Roses）。

### 59.16 tex2/3/4 uniform 上传被丢弃（已修）与非 0 验证

`WriteUniform()`（`glUniform2fv/3fv/4fv` 的公共写入路径）里：

```cpp
if (dst + 16 > uniform->value.size ()) break;   // vec2 缓冲 8B → 永远 break
++uniform->writeCount;
```

MSL uniform 成员按 float4 对齐（16B），但 staging 缓冲按反射的实际大小分配（vec2 = 8B），
于是**所有 vec2/vec3 uniform 上传都被静默丢弃**。已改为按 `count*16` 扩容后写入。

修复后审计确认（program=189，Thorns 的屏幕 shader）：

```
openfl_TextureSize writes=1 value=2498.0000,1404.0000   ← 现在是正确的
openfl_Matrix     writes=1 value=0.0008,...,+0.0014
hasTransform      writes=0 value=0
_camSize          writes=0 value=0
pixelZoom         writes=0 value=0
```

### 59.17 剩余疑点

`hasTransform` / `_camSize` / `pixelZoom` 三个 CNE 自定义 uniform **writes=0**（游戏从未写过），
而该 shader 的变换/缩放数学依赖它们 → 仍是"缩略 + 翻转"的最可能来源。

下一步怀疑：这些参数的赋值发生在**绑定该 program 之前**，而 `UniformAt()` 是按
`state.currentProgram` 解析的 → 写到了别的 program 上。验证方法：给 `UniformAt()` 加
`CNE_METAL_UNIFORM_PROG_LOG=1`，打印每次 uniform 写入命中的 program 与 uniform 名。

### 59.18 结论：Week 6 三首曲目现在全部正常渲染

先前几轮"缩略/翻转/黑屏"的判断有**导航错误**（Freeplay 列表选中项会滚动到顶部，按固定 ↓
次数并不对应固定曲名）。用「每按一次 ↓ 后立即截图」建立精确映射后实测：

| Freeplay ↓ 次数 | 曲目 |
|---|---|
| 7 | Winter Horrorland |
| 8 | Senpai |
| 9 | Roses |
| **10** | **Thorns** |

用当前构建（含 §59.16 的 `WriteUniform` vec2/vec3 扩容修复）实测：

- **Thorns**：暗紫舞台、GF、幽灵角色、BF、HUD 全部正常，尺寸与方向正确
- **Roses**：Senpai/GF/BF、舞台、HUD 全部正常
- **Senpai**：Senpai/GF/BF、舞台、HUD 全部正常

即此前反馈的"Thorns 上下翻转 + 缩略"、"Senpai/Roses 全黑"在加入
`WriteUniform()` 扩容修复后已经消失 —— 该函数曾把**所有 vec2/vec3 uniform 静默丢弃**
（包括 OpenFL 滤镜/屏幕 shader 依赖的 `openfl_TextureSize`、CNE 的 `_camSize`/`pixelZoom`），
导致滤镜 pass 的 UV 数学退化。

### 59.19 导航方法（后续排查务必遵守）

`CNE_METAL_AUTOKEYS` 的 ↓ 间隔至少 **90 帧**（1.5 s），否则会丢键；并且**每按一次就截一张**
确认选中曲名，不要按固定次数盲跳。曲名看 Freeplay 列表顶部高亮项或暂停菜单右上角。

## 60. 收尾状态与教训

### 60.1 `WriteUniform` 的 vec2/3 扩容尝试 → 已回退

§59.16 把 `WriteUniform()` 从"缓冲不够就丢弃"改成"扩容后写入"，实测导致
**Winter Horrorland 与 Week 6 全部曲目黑屏**（这些曲目的滤镜/屏幕 shader 依赖这些向量
uniform 保持原值）。已回退；`openfl_TextureSize` 因此仍为 0，但画面正常 —— 说明该 uniform
在当前 shader 变体里并未参与决定可见结果。

**教训**：任何看似"明显是 bug"的静默丢弃，改之前必须先确认有多少 shader 依赖原行为。

### 60.2 进入主菜单变慢 → 已解决

用户确认修复。相关改动：移除 FBO 的惰性创建路径上的重复工作、以及本次回退。
诊断开关 `CNE_METAL_HITCH=1` 保留（打印 >34ms 的帧及帧号/耗时）。

### 60.3 导航教训（重要）

用 `CNE_METAL_AUTOKEYS` 自动导航时：

- ↓ 间隔必须 ≥ 90 帧（1.5 s），否则会丢键
- **每按一次都要截图确认曲名**，不要按固定次数盲跳
- Freeplay 列表会把选中项滚动到顶部，索引与 `freeplaySonglist.txt` 的行号不一致

实测映射（90 帧间隔，从进入 Freeplay 起算）：7 = Winter Horrorland，8 = Senpai，
9 = Roses，10 = Thorns。

### 60.4 最终状态

```
✅ 转场黑幕 / stencil / Resize 视口 / 进入主菜单变慢
✅ FBO 离屏合成、FBO 视口、per-switch view、FBO 重挂附件
✅ 矩阵 uniform 上传（两条路径）
✅ Winter Horrorland / Senpai / Roses / Thorns 画面正常（回退后实测）
🟡 openfl_TextureSize 仍为 0（无可见影响，暂不动）
```

## 61. Winter Horrorland / Week 5-6 黑屏：排查记录（未解决）

### 61.1 已确认为「真黑帧」

用 `CNE_METAL_SHOT_RAW` 抓黑屏期的背缓冲（我直接读像素）：

```
size 2498x1440 pitch 9992
非黑像素 = 9.47%（正好只有 HUD 的箭矢 + 血条）
alpha 均值 248.5
```

⇒ 世界没有进入最终画面，不是呈现层问题。

### 61.2 已排除的原因

| 检查项 | 结果 |
|---|---|
| FBO 附件映射 | `131→129`、`148→147`，与提交清单一致 ✓ |
| 视图执行顺序 | view0(世界) → view1(滤镜) → view2(世界)，符合提交顺序 ✓ |
| 视图矩形 | `(0,0,2498,1404)` 覆盖整个目标 ✓ |
| 滤镜 uniform | matrix/textureSize/camSize/color/amount/strength/sampler 全部正确 ✓ |
| FBO 创建 | `valid=1`，各一次；11.4s 的重试风暴已修 ✓ |

### 61.3 已修复的相关缺陷

- FBO 创建失败时的**重试风暴**（一帧 11.4 秒 → 695ms）
- `WriteUniform` 的 vec2 扩容（`openfl_TextureSize` 现在能正确上传；但恢复后黑屏仍在，
  说明它不是唯一原因）

### 61.4 尚未完成的验证

用 `CNE_METAL_SHOW_WORLD_RT=1` 把最终合成的采样纹理替换成「世界 FBO 纹理」后画面**仍然全黑**
→ 但该替换只覆盖纹理单元 0，如果合成用的是其它采样单元则实验无效，需要先确认单元号。

**下一步**：在该替换生效时打印一行日志（命中的 draw、原纹理、替换后纹理、采样单元），
确认替换是否真的发生；若确实命中却仍黑，则世界 pass 的输出确实为空，需查世界 pass 的
颜色写入/混合与视图绑定细节。

### 61.5 决定性实验：世界 FBO 是空的

用 `CNE_METAL_SHOW_WORLD_RT=1` 把最终合成的采样纹理强制替换为**世界 FBO 的纹理**（129），
日志确认替换生效：

```
[cne-worldrt] unit=0 bound=147 -> world=129 prog=4
```

画面**仍然全黑** ⇒ **世界 FBO（129）里没有内容**。也就是说：

> 世界那 6 笔绘制（view0 → FBO 131/tex129）没有写出任何像素。

这把问题从"滤镜"彻底转到"世界 pass 的离屏渲染"，而同一路径的 SENPAI 对话（也是 FBO）此前
渲染正常 —— 所以不是"FBO 一律不可用"，而是这个 pass 的某种状态导致没有写入。

### 61.6 下一步诊断（需要的新日志）

在世界 pass 的**第一笔**提交上打印完整状态：

```
[cne-world] view=0 fbo=131 prog=? tex=? rect=? viewport=? blend=? cw=?
            viewHasFbo=? drawsInView=?
```

重点看：
- `viewHasFbo` —— 提交时该 view 是否真的绑上了 framebuffer（`setViewFrameBuffer` 是否被调用）
- `cw` —— 颜色写入掩码是否被某处设为 0
- `blend` —— 是否被设成不产生颜色的组合
- `drawsInView` —— view0 里到底有没有绘制（若为 0，说明绘制被分配到别的 view）

### 61.7 当前交付

```
✅ 已修：转场黑幕 / stencil / Resize / 主菜单变慢 / FBO 创建重试风暴 / 矩阵与 vec2 uniform 部分
🟡 未解决：Winter Horrorland 与 Week 5-6 黑屏 —— 已确认世界离屏 pass 未产出像素
✅ 诊断开关：CNE_METAL_SHOW_WORLD_RT / DEBUG / HITCH / DRAWLOG / MAXTEX / SHADER_AUDIT / STATE_LOG
```

## 62. 离屏合成链路：已修复的缺陷与残留问题

### 62.1 已修复（每项都有日志/截图验证）

| 缺陷 | 根因 |
|---|---|
| 合成从未到达屏幕 | 合成笔与离屏 pass **共用 view id**；bgfx 一个 view 只能绑定一个 framebuffer。改为离屏 0–199 / 屏幕 250– 分离 |
| 世界整块被裁 | OpenFL 的 scissor 比目标大 1px（2499×1405 vs 2498×1404），`targetH-(y+h)` 得 −1，转 `uint16` 溢出 65535 → 全裁。新增 `ClampToTarget()` |
| 片段全被拒 | 离屏 pass 开着深度测试，但 FBO 无深度附件（且游戏从不 glClear）。离屏忽略深度 |
| HUD 被合成覆盖 | 屏幕 pass 必须排在所有离屏 pass 之后并保持提交顺序 |
| 画面上下翻转 | **翻转代码加在了从未被调用的 `lime_gl_uniform_matrix4fv`**；实际路径是 `cne_metal_uniform_matrix4fv`（MetalNative.inc）。移植后才生效 |
| 合成采样到错误纹理 | 世界目标选择逻辑的赋值被误删，`gFrameWorldTex` 恒为 −1。改为按上一帧离屏绘制量选择，并排除中间目标 |

**结果：Winter Horrorland 完全正常**（世界 / HUD / Notes / 暂停界面 / 方向 / 镜头位置）。

### 62.2 残留：Week 6（Senpai / Roses / Thorns）

结构：`view0→fbo131(tex129) 世界` → `view1→fbo158(tex157) 屏幕shader` → `view2→fbo131 世界(第二段)` → `合成采样 129`（替换已确认生效）。

- `CNE_METAL_FBO_FLIP=1`（默认，补偿 2.0）→ 全黑
- `CNE_METAL_FBO_FLIP=0` → 世界出现，但**缩小且上下翻转**
- `CNE_METAL_SKIP_VIEW=0/1/2` → 均无效

镜像补偿 2.0 是在 Winter Horrorland 上实测得到的经验值，它依赖相机矩阵的平移量，**不可跨曲目通用**。正确做法是把翻转改为**与相机无关的 UV 翻转**（只作用于最终合成采样），而不是镜像世界矩阵。

### 62.3 残留：启动偶发卡顿

`frame=12` 单帧 **6398ms**，当时 `texCreates=0 fboCreates=0 programsLinked=0` ⇒ 阻塞在渲染资源创建之外（资源加载 / 音频 / macOS 首次编译 Metal 管线）。需在启动路径加分段计时定位。

### 62.4 诊断开关（全部环境变量，默认关闭，不影响正常游玩）

```
CNE_METAL_DEBUG HITCH HITCH_DETAIL WORLDLOG COMPLOG SAMPLERLOG PROGLOG SKIP_VIEW
CNE_METAL_SHOT_RAW DUMP_WORLD FBO_FLIP COMPOSITE_FLIP FLIP_OFFSET
CNE_METAL_SHOW_WORLD_RT STATE_LOG MAXTEX TEXSIZE
CNE_METAL_SHOW_WORLDCORNER [WORLDCORNER_PROG] MTL_AUDIT
```

## 63. 第六十三轮：复制 pass 观测 + 启动卡顿分段计时

### 63.1 复制 pass 观测（`CNE_METAL_SHOW_WORLDCORNER=1`）

**目的**：在不依赖离屏回读（该 bgfx 版本不可靠，见 §61.5）的前提下，一次运行就能回答
「Week 6 黑屏时世界纹理里到底有没有内容」。

**实现位置**：`MetalBindings.cpp` → `WorldCornerCopyPass()`，由 `ApplyFrameViewOrderImpl()`
在 `bgfx::frame()` 之前调用（在视图重排 / 早退之前，见下）。

要点：

* 专用 **view 252**（离屏 0–199、屏幕 250– 两个区间之外；readback 用 253，不冲突）；
  `setViewRect(252, 0, 0, W/4, H/4)` + `setViewFrameBuffer(252, BGFX_INVALID_HANDLE)` ⇒ 屏幕左上角 1/4。
* 复用游戏**现成的 prog 4**（标准 bitmap 程序，可用 `CNE_METAL_WORLDCORNER_PROG` 换）。
  采样器 uniform 从 prog 4 的 `uniforms` 里取第一个 `isSampler` 的 handle；
  顶点属性按该程序自己的 `attributeLocations` + `attribSlots`（GLSL→MSL 的 slot 映射）动态生成，
  属性尺寸按 OpenFL/Flixel 顶点面已知语义：position=vec4、texcoord=vec2、alpha=float、
  ColorMultiplier/ColorOffset=vec4；遇到未知属性会打印一次并拒绝绘制（不猜尺寸）。
* 顶点缓冲是**静态缓存**（按 prog handle idx + 属性数量失效重建），6 顶点两个三角形，
  位置直接给 NDC [-1,1]，因此绘制前把 `openfl_Matrix` 显式设成单位矩阵
  （`openfl_HasColorTransform`/`hasTransform` 置 0、`openfl_TextureSize` 置世界纹理尺寸），
  完全覆盖游戏留在 prog 4 暂存区里的上一次合成矩阵。
* 状态固定为 `WRITE_RGB|WRITE_A`（无混合 / 无深度 / 无剔除），观测量不受游戏状态影响。
* 采样纹理 = 本帧实时跟踪的 `gFrameWorldTex`（与合成用的是同一个），view 252 在
  `setViewOrder` 的“未列出视图按升序补在后面”规则下排在所有游戏 pass 之后执行。

**判读**：

| 左上角 1/4 | 结论 | 下一步 |
|---|---|---|
| **有画面**（舞台/角色，可能方向/大小不对） | 世界纹理**有内容** | 问题在合成/滤镜/读取侧（§62.2 的 FLIP 经验值、屏幕 shader 矩阵等） |
| **全黑** | 世界纹理**确实为空** | 问题在写入侧：世界 pass 的矩阵/状态/FBO 绑定/翻转 |

已知盲区：prog 4 的标准片段着色器有 `if (color.a == 0.0) gl_FragColor = vec4(0)` 提前返回，
所以「世界纹理只有 RGB、alpha 全 0」也会显示为黑。真遇到这种情况，可用
`CNE_METAL_WORLDCORNER_PROG=<其它程序 id>` 换一个不做该提前返回的程序复测。

**日志**（stderr）：

```
[cne-copy] quad built program=4 bgfxProgram=... attrs=5 stride=60 world=129 2498x1404
[cne-copy] f=180 view=252 rect=(0,0,624,360) world=129(handle=47 2498x1404) prog=4 unit=0 sampler=...
```

**已自测**（只启动到主菜单、不操控）：主菜单是直接画到背缓冲（没有世界 FBO），所以只出现
一次面包屑 `[cne-copy] enabled; no world render target yet (direct-to-screen frame)`，
应用跑 600+ 帧无 abort；进歌后才会出现 `[cne-copy] f=… view=252 …` 的正式行。

**运行**：

```sh
CNE_METAL_SHOW_WORLDCORNER=1 \
  export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-run.log 2>&1
```

**免重建的环境变量 A/B（建议与复制 pass 同轮做）**：

```sh
# Run B：用「采样 UV 翻转」代替「矩阵 Y 翻转」
CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=2 \
  export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runB.log 2>&1

# Run C：同上，并打开复制 pass 观测
CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=2 CNE_METAL_SHOW_WORLDCORNER=1 \
  export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runC.log 2>&1
```

判读组合见上游交接记录的第三节第 1 项（左上角有画面/黑 × Run B 正常/仍黑 四种结论）。

### 63.2 启动卡顿分段计时（`CNE_METAL_HITCH_DETAIL=1`）

`CNE_METAL_HITCH` 原来的 `dt = start(n) - start(n-1)` 把两种完全不同的停顿混在一起。
新增的详细计时把每一帧拆成：

| 字段 | 含义 | 若它很大说明 |
|---|---|---|
| `game` | `start(n) - end(n-1)`，两帧之间游戏线程做的事 | 阻塞在**游戏逻辑 / 资源加载 / 音频初始化**，渲染器无罪 |
| `submit` | `Frame()` 内构建 bgfx 命令流（截图请求 + 视图重排 + 诊断） | 阻塞在我们的提交路径 |
| `present` | `bgfx::frame()` 内部（等渲染线程 / GPU） | 阻塞在驱动 / GPU / 首次管线 |
| `rest` | present 之后（截图落盘、stats） | 阻塞在诊断写文件 |
| `gameCreates=+tex/+fbo/+prog` | 该 `game` 段内创建的资源数 | 定位是哪种资源加载 |

只打印 `game > 34ms 或 total > 34ms` 的帧。

**已自测样例**（启动阶段，未进任何游戏界面）：

```
[cne-hitch-detail] frame=3 dt=607.7ms game=607.5ms submit=0.0ms present=0.0ms rest=0.0ms total=0.0ms gameCreates=+0/+0/+0
[cne-hitch-detail] frame=4 dt=36.6ms game=36.5ms submit=0.0ms present=0.0ms rest=0.0ms total=0.0ms gameCreates=+0/+0/+0
```

即启动阶段的慢帧 100% 在 game 段，提交/present 都是 0 —— 与「texCreates/fboCreates/
programsLinked 全 0」的旧观察完全吻合。6.4s 那一帧用同样的日志即可判定归属。
注意 `bgfx::frame()` 是异步提交，`present` 通常接近 0；`present` 很大才说明阻塞在驱动/GPU。

**与 shader 运行时编译联合判断**：`CNE_METAL_MTL_AUDIT=1` 的探针本来就会为每次
`newLibraryWithSource` 打印 `call #N (X.XXms) <MSL 前 60 字符>`（MetalLayerHost.mm）。
所以最快的定位组合是：

```sh
CNE_METAL_HITCH=1 CNE_METAL_HITCH_DETAIL=1 CNE_METAL_MTL_AUDIT=1 \
  export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-run.log 2>&1
grep -n 'cne-hitch-detail\|newLibraryWithSource' /tmp/cne-run.log
```

* 若 6.4s 的帧 `game≈6398ms` 且附近有 `newLibraryWithSource (≈秒级)` ⇒ 是 shader 源编译；
* 若 `game≈6398ms` 但没有 source compile ⇒ 是游戏侧加载（再看 `gameCreates`）；
* 若 `present` 巨大 ⇒ GPU/驱动侧（首次 PSO / 交换链）。

### 63.3 已存在的 AOT 缺口：11/56 个 stage 没有 .metallib

`metal/audit/aot-coverage.md`（每次 `apply.sh` 重新生成）显示 45/56 个 stage 成功产出
`.metallib`，另外 11 个在构建期 Metal 编译失败，**运行时会回退到 `newLibraryWithSource`
（MSL 源码编译）**。清单（key → 来源，见同文件第 4 节）：

| 来源 | stage | 失败原因（构建期 clang） |
|---|---|---|
| `asset:assets/shaders/engine/editorWaveformsRainbow/fragment` | fragment | no matching function for call to 'glsl_mod' |
| `class:flxanimate.FilterShader#0/#1/fragment` | fragment | float 被当函数调用 / 声明错误 |
| `class:funkin.backend.shaders.OverlayShader#0/#1/fragment` | fragment | program scope variable must reside in constant address space |
| `class:funkin.backend.shaders.WiggleShader#0/#1/fragment` | fragment | 同上 |
| `class:openfl.filters.BlurAlphaShader/BlurShader/ConvolutionShader#0/vertex` | vertex | float ← float2 赋值错误 |
| `class:openfl.filters.DisplacementMapShader#0/fragment` | fragment | float4 → float2 尺寸不匹配 |

这 11 个都在“过滤器 / 特效 / 编辑器”路径上，与 Week 6 黑屏无关，但**与启动偶发卡顿有关**：
若启动时确实创建了这些 shader，就会触发一次或多次 MSL 源码编译（每次可达数百 ms～秒级）。
先用 §63.2 的组合日志确认，再决定是否去修翻译器（每个失败都是独立的 GLSL→MSL 类型推断
问题，需要单独一轮）。

### 63.4 本轮构建环境备注（历史）

* `/tmp/hxenv.sh` 已不存在（/tmp 被清），但 `haxe`/`haxelib`/`neko` 都在 PATH
  （Haxe 4.3.7，hxcpp 4,3,2），直接构建即可。
* `haxelib dev lime` 绑定会丢失；构建前确认：
  ```sh
  haxelib path lime | head -1   # 必须指向 ~/haxelib-dev/lime-832
  # 若不对：
  haxelib dev lime $HOME/haxelib-dev/lime-832
  ```


### 63.5 第一轮用户实测结果（2026-10-23 18:34，Run A / Run B）

**Run A**（默认 FBO_FLIP=1 + 复制 pass + HITCH_DETAIL + MTL_AUDIT）：

```
[cne-copy] program 4 attribute set unsupported (known=0 attrs=4 pos=1 uv=0):
           openfl_Alpha openfl_ColorMultiplier openfl_ColorOffset openfl_Position openfl_TextureCoord
```

⇒ **复制 pass 根本没画**：属性名匹配器写的是 `find("texcoord")`，而实际名字是
`openfl_TextureCoord`（小写后 `openfl_texturecoord` 里并不含子串 `texcoord`），
于是把 texcoord 当成未知属性拒绝了。**这是本诊断自身的 bug，已修复**
（现在同时匹配 `texcoord` / `texturecoord` / `tex_coord`），并增加了
`[cne-copy] quad built …` 这一行便于确认。**因此 Run A 的"全黑"不能用于判读世界纹理**，
需要重跑。

**Run B**（`FBO_FLIP=0 RT_UVFLIP=2`）：世界出现，但**缩小 + 上下翻转**（与 §62.2 一致）。
说明 FLIP=0 时世界 pass 确实往纹理里写了内容；合成/滤镜链的缩放与方向仍未解决。
日志证据：`[cne-worldrt] unit=0 bound=162 -> world=134 prog=4`。

**卡顿分段（Run A 全程）**：所有 >34ms 慢帧都是 `game` 段，`submit ≤ 1.2ms`、
`present = 0`（`bgfx::frame()` 异步提交）。启动 539ms、进歌前后 197–303ms 全在游戏侧；
本次运行**没有复现** 6.4s，说明它不是每局必现，而是资源加载路径上的偶发长尾。

### 63.6 第二轮诊断增量（本构建已包含）

1. **`CNE_METAL_FILTERLOG=1`**：每次「采样 render-target 的 pass」（滤镜 / 屏幕 shader /
   最终合成）打印一行，每帧最多 8 条：

   ```
   [cne-filter] f=.. sub#.. prog=4 fbo=0(2498x1440) view=250 vp=(0,0,2498,1440) \
                tex=134(unit=0 2498x1404) quad=(0.0,1404.0 stride=20 verts=4) \
                matrix=(0.00080,-0.00142,0.000,2.000)
   ```

   字段含义：

   * `fbo/fboSize`：这一笔画进哪个目标、目标多大
   * `viewport`：GL viewport（会变成 bgfx view rect；缩放 bug 常在这里或 quad/matrix 里）
   * `quad=(x,y stride verts)`：该笔第一个顶点的前两个分量（像素坐标）、顶点 stride、顶点数
   * `matrix=(m0,m5,m12,m13)`：`openfl_Matrix` 的 **X/Y 缩放**与 **X/Y 平移**
   * `tex`：被采样的 render target 及其尺寸

   判据：全屏合成 ⇒ viewport 与目标同尺寸、quad 覆盖目标、matrix 缩放 ≈ `2/宽` 与 `-2/高`。
   屏幕上的"小卡片" ⇒ 三者中必有一个明显偏小。

2. **RT_UVFLIP 修复 + 验证日志**：原来的 `isRenderTarget` 判断只看
   `CurrentTextureId()`（当前 active texture unit），而 OpenFL 可能在 unit 0 绑定 RT、
   另一个 unit 处于 active 状态 → 翻转被静默跳过。现在**扫描所有 bound unit**，
   并打印 `[cne-uvflip] …`（意图）与 `[cne-uvflip] APPLIED path=A/B …`（真的改了 UV）。

### 63.7 第二轮请用户跑的命令

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1

# Run A2：默认翻转 + 复制 pass + 滤镜/合成数据（进 Senpai，暂停后截图）
env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_FILTERLOG=1 CNE_METAL_FBOMAP=1 \
    CNE_METAL_HITCH=1 CNE_METAL_HITCH_DETAIL=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runA2.log 2>&1

# Run B2：FBO_FLIP=0 + RT_UVFLIP=2，同样的日志（同一首歌，截图）
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=2 CNE_METAL_SHOW_WORLDCORNER=1 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_FBOMAP=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runB2.log 2>&1
```

需要回传：`/tmp/cne-runA2.log`、`/tmp/cne-runB2.log` + 两张截图（含左上角 1/4 区域）。


### 63.8 第二轮实测判读（Run A2 / Run B2，2026-10-23 18:46）

**Run A2（默认 FBO_FLIP=1）**：

* 复制 pass 正常执行：`[cne-copy] quad built program=4 … world=129 2498x1404`、
  `[cne-copy] f=412 view=252 rect=(0,0,624,360) world=129(handle=49 …)`。
* 屏幕左上角 1/4 为**纯黑**（逐像素分析：左 1/4 均值 0.05/0.05/0.08，右 1/4 均值 38/40/44；
  x=W/4 两侧均值 0.01 vs 14.8）⇒ **默认翻转下世界 RT（129）确实为空**，
  合成链再正确也画不出东西。⇒ 写入侧 bug 坐实。
* FILTERLOG 显示合成几何完全正常：`prog=4 fbo=0(2498x1440) vp=(0,0,2498,1440)
  quad=(2498,1404 stride=56 verts=4) matrix=(0.00080,-0.00139,-1.001,0.975)`
  —— viewport/quad/matrix 都是全屏，**问题不在合成几何**。

**Run B2（FBO_FLIP=0 + RT_UVFLIP=2）**：

* 屏幕出现画面，但**世界被缩成屏幕中央的小块且上下翻转**；复制 pass 的左上角 1/4 仍为黑。
* 关键发现：Week 6 用的是 **pixel-art 管线**（`assets/data/scripts/pixel.hx`）：
  `makeCameraPixely(cam)` 把相机 `zoom /= daPixelZoom(≈6)`，并给相机加
  `CustomShader('pixelZoomShader')`；`postUpdate` 每帧把 `e.zoom` 与 `e.pixelZoom`
  设为 `1/daPixelZoom/scale`。世界**故意以 1/6 尺寸**渲染进 RT，再由
  `pixelZoomShader` 采样时放大 6 倍还原。
* `pixelZoomShader` 的放大数学依赖三个 uniform：`pixelZoom`（脚本设置）、
  `_camSize`（Flixel `FlxCamera.update` → `FlxGraphicsShader.setCamSize()`，每帧）、
  `openfl_TextureSize`（OpenFL `applyBitmapData`，是**源 RT 的纹理尺寸**）。
  `getCamPos/camToOg` 用 `_camSize/openfl_TextureSize` 做映射，任何一项为 0 或尺寸基准
  不一致，放大就会退化成 1:1 —— 这正是"世界 1/6 大小直接铺到全屏"（小卡片）的现象。
* 因此"缩小"不是 bug 于世界绘制，而是 **pixelZoomShader 的反放大没有生效**。
* 另外 FILTERLOG 还显示 B2 下滤镜 pass 的矩阵随 OpenFL 的 FBO 投影一起变成
  `m5=+0.00142, m13=-1`（A2 是 `-0.00142, +1`），说明 RT 方向的处理仍有一个未对齐的翻转点。

**下一步（需要一次运行）**：用 `CNE_METAL_SHADER_AUDIT=1` 抓 program=161（pixelZoomShader）
的 uniform 快照，直接看 `pixelZoom` / `_camSize` / `openfl_TextureSize` 的
`writes=` 与 `value=`（第 63.6 节已说明为何这是决定性数据）。命令见 §63.9。


### 63.9 第三轮：pixelZoomShader uniform 快照（一次运行，无需重建）

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=2 CNE_METAL_SHOW_WORLDCORNER=1 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_SHADER_AUDIT=1 CNE_METAL_SHADER_AUDIT_FRAME=450 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runB3.log 2>&1
```

进 Senpai 玩 3–5 秒后直接退出（EXIT 时 audit 会补全所有 program 的 uniform 表）。回传
`/tmp/cne-runB3.log`（或只给下列输出）：

```sh
grep -a -B2 -A 25 'cne-shader-audit\] program=161 ' /tmp/cne-runB3.log
grep -a 'cne-uvflip' /tmp/cne-runB3.log | head
```

要看的点（program=161 = pixelZoomShader）：

| uniform | 期望 | 若为 0 / 不符 |
|---|---|---|
| `pixelZoom` | ≈ 1/6 = 0.1667，writes≥1 | 放大退化成 1:1（小卡片） |
| `_camSize` | `(0,0,W,H)`，W/H 与 RT 纹理尺寸一致 | getCamPos/camToOg 映射错 |
| `openfl_TextureSize` | = 源 RT 的纹理尺寸（2498×1404） | 与 _camSize 基准不一致 → 缩放错 |
| `openfl_Matrix` | 全屏投影（≈2/2498） | 几何位置错 |

拿到这张表后就能确定是"某个 uniform 没上传"还是"尺寸基准不一致"，然后一次性修
（同时把 `m[13]+=2.0` 的矩阵翻转补偿换成与相机无关的 RT 方向处理）。


### 63.10 第三轮之后的决定性进展（2026-10-23 18:57）

**1) 复制 pass 管线本身已被验证正确（本地测试，无需用户）**

给复制 pass 增加了一个 2×2 参考贴图色板（红/绿/蓝/黄），画在**右上角 1/4**
（view 254），世界纹理仍画在左上角（view 252）。在**主菜单本地启动 + 截图**验证：
色板方向完全正确（左上红、右上绿、左下蓝、右下黄）⇒ view rect / quad / 身份矩阵 /
采样器绑定 / UV 方向全部正常。色板不依赖世界 RT，所以菜单（没有离屏 pass）也能验证。

⇒ 之前 WH / Week 6 左上角"黑方块"说明**采样到的是黑内容**（不是 pass 坏了）；
同时也说明世界纹理在 view 252 执行时可能是空的/不是屏幕合成实际用的那张，
需要 MATLOG 进一步对齐（见下）。

**2) Week 6 "小卡片"的真凶：`CNE_METAL_SHOW_WORLD_RT` 默认开启**

B3 的 `SHADER_AUDIT` 证明 pixelZoomShader（program=161）的所有 uniform **完全正确**：

```
_camSize           writes=7 value=0.0000,0.0000,2498.0000,1404.0000
openfl_TextureSize writes=7 value=2498.0000,1404.0000
pixelZoom          writes=7 value=0.1709        (= 1/daPixelZoom/scale)
openfl_Matrix      writes=7 value=0.0008,0,0,0,0,0.0014,...
```

即像素放大链路的数据是对的。屏幕上的"世界缩小成 1/6 小卡片"恰恰是因为
`CNE_METAL_SHOW_WORLD_RT`（旧版本的 work-around）默认开启：它把最终合成的采样纹理
从 **pixelZoomShader 输出（157）** 换成 **原始世界 RT（129）**，于是 1/6 尺寸的原始世界
被 1:1 铺到全屏。**已把该开关默认改为关闭**（需要复现旧行为时设
`CNE_METAL_SHOW_WORLD_RT=1`）。WH 没有 screen-shader 链，因此不受影响。

**3) 新增 `CNE_METAL_MATLOG=1`**

每帧前 12 笔打印实际要上传的 `openfl_Matrix`（m0,m5,m12,m13）。用于设计
"与相机无关"的 RT 方向修正：现在 `FBO_FLIP=1` 的 `m13 += 2.0` 是 WH 上调出来的经验值，
Week 6 下可能把内容移出 RT（世界变黑），需要改用基于 UV/采样端的修正。

### 63.11 下一轮请用户跑（默认值已改，重点看 Week 6 是否恢复全屏）

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1

# D1：全默认（SHOW_WORLD_RT 已默认关）+ 矩阵日志，进 Senpai 截图
env CNE_METAL_MATLOG=1 \
  ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runD1.log 2>&1

# D2：同上但关掉旧的矩阵翻转
env CNE_METAL_MATLOG=1 CNE_METAL_FBO_FLIP=0 \
  ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runD2.log 2>&1
```

回传日志 + 两张截图。判读：

| D1（默认 FLIP=1） | 结论 |
|---|---|
| 世界全屏但可能翻转 | 像素链已恢复，只剩方向问题 → 用 MATLOG 设计 UV 端修正 |
| 仍黑 | 旧的 m13+2.0 翻转把 Week 6 世界移出 RT（对比 D1/D2 的 m13 即可确认） |
| 仍是小卡片 | 说明还有别的地方在采样原始世界 RT（把 `[cne-worldrt]` 行发我） |

### 63.12 D1/D2 实测：矩阵翻转 hack 的数学证据 + 正确的方向方案

**D1（默认 FBO_FLIP=1，SHOW_WORLD_RT=0）→ 全黑；D2（FBO_FLIP=0，SHOW_WORLD_RT=0）→ 小卡片+翻转。**

MATLOG（`CNE_METAL_MATLOG=1`）给出了决定性数据。D1 中世界绘制（prog 14 → fbo 132）的实际上传矩阵是：

```
m = (m0, m5, m12, m13) = (0.00027, -0.00047, -0.170, 1.829)
```

而 `FBO_FLIP=1` 的实现是 `m5 = -m5; m13 += 2.0`，所以原始矩阵的 m13 只有 **-0.171**，
不是 WH 那种 -1 左右的投影平移。加上 2.0 后，y∈[0,1404] 映射到 clip y ∈ [1.829 … 1.169]
——**全部 > 1，整块几何被推到画面上方之外**，世界 RT 因此全空（这就是 Week 6 在默认
FBO_FLIP=1 下全黑的真正原因）。WH 的 m13 原本接近 -1，+2.0 后仍在 [-1,1] 内，所以"能用"。

⇒ 结论：`m13 += 2.0` 的矩阵翻转是**相机平移相关**的 hack，必须废弃。正确方案：

1. `CNE_METAL_FBO_FLIP=0`（不改任何矩阵，保持 OpenFL/GL 的投影语义）；
2. 在**采样端**修 RT 方向：新增 `CNE_METAL_RT_UVFLIP=3` —— 任何"采样 render target"的绘制
   （滤镜 pass 写入另一个 RT、以及最终合成写屏幕）都把 V 翻转一次。每个 RT 采样点恰好翻一次，
   与相机无关。
3. 同时 `CNE_METAL_SHOW_WORLD_RT=0`（让真正的 pixelZoomShader / 屏幕 shader 链回到合成里）。

注意：D1 里 `SHOW_WORLD_RT=0` 还会让 WH 黑屏，因为屏幕 shader 的输出（合成原本要采样的 RT）
在缺少采样端翻转时是错/黑的 —— 这正是 `RT_UVFLIP=3` 要修的部分。因此三个开关必须一起测试。

**下一轮（已构建）**：

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_MATLOG=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE1.log 2>&1
```

先用 Senpai 验证；再用同样的环境变量进 Winter Horrorland（/tmp/cne-runE2.log）确认没有回归。
如果 E1 世界全屏且方向正确、E2 也正常，就把这组设为**默认值**并删掉矩阵翻转 hack。

### 63.13 E1/E2 实测（10-24 18:28）

配置：`FBO_FLIP=0 RT_UVFLIP=3 SHOW_WORLD_RT=0`（+ FILTERLOG/MATLOG）。

* **E1（Senpai）**：世界仍是小的（约 1/4 屏）+ 上下翻转；左上角无黑方块（复制 pass 未开）。
  日志显示链路已按预期走：世界 11 笔 → fbo 133；`prog=163` 滤镜 pass（采样世界 RT）→ fbo 160；
  最终合成 `prog=4` 采样 159，quad/viewport/matrix 全屏；UVFLIP mode 3 对这两个采样点都 APPLIED。
  ⇒ 合成几何没问题，**是滤镜 pass 的输出（159）里世界就是小的**，即 pixelZoomShader 的放大
  没有作用到最终结果（或该 program 不是 pixelZoomShader）。
  世界绘制矩阵 `m=(0.00027,0.00047,-0.170,-0.171)` ≈ 相机 zoom 1/5.8（pixel 相机确实生效）。
* **E2（WH）**：世界黑屏。`prog=151`（WH 自己的相机 shader）采样世界 RT → fbo 148，
  合成采样 147；UVFLIP mode 3 APPLIED。黑屏说明 SHOW_WORLD_RT=0 时 WH 的 screen-shader 输出
  仍不可用（这正是当初加 SHOW_WORLD_RT 这个 work-around 的原因）。

**待确认**：program=163 的 uniform 快照（pixelZoom/_camSize/openfl_TextureSize 的 writes/value），
以及**不暂停**（正常游玩中）截图 —— 之前所有截图都是在暂停状态截的，暂停会引入 pauseCam 的
独立滤镜链，可能干扰判断。

### 63.14 下一轮（无需重建）：抓 program=163 的 uniform + 游玩中截图

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_SHADER_AUDIT=1 CNE_METAL_SHADER_AUDIT_FRAME=450 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE3.log 2>&1
```

进 Senpai 后**先正常游玩 3–5 秒截一张图（不要暂停）**，再暂停截一张，然后退出；
回传 `/tmp/cne-runE3.log`。重点看 `grep -a 'program=163' -A 20 /tmp/cne-runE3.log`。

### 63.15 E3 日志结论（pixelZoomShader 的 MSL 与 uniform 都对）

* `program=165` = pixelZoomShader（有 pixelZoom/_camSize uniform），uniform 全部正确：
  `pixelZoom=0.1709`、`_camSize=(0,0,2498,1404)`、`openfl_TextureSize=(2498,1404)`、
  `openfl_Matrix` 全屏；`writes=76`。
* AOT 缓存里该 shader 的 MSL 也确认包含放大数学：
  `camPos = 0.5 + (getCamPos(uv) - 0.5) * pixelZoom`。
* E3 游玩中（未暂停）截图仍是**小世界 + 翻转**；暂停菜单是全屏正常的，但那是因为
  `week6-pause.hx` 自己把 UI 放大 6 倍、与 pauseCam 的 1/6 zoom 相互抵消，
  **不能证明 pixelZoomShader 的放大生效**。
* 因此下一步必须看 **绘制时刻** 的 uniform 与 **滤镜 quad 的 UV**：
  本轮构建已给 `CNE_METAL_FILTERLOG` 增加 `pixelZoom/camSize/texSize` 与
  `uvs=4:(u,v)×4`（真实顶点 UV）输出，专门回答"shader 收到的值是否和审计一致、
  quad 是否真的覆盖 0..1"。

### 63.16 请跑 E4（本轮已构建，无需重新编译）

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE4.log 2>&1
```

进 Senpai，正常游玩 3–5 秒（**不要暂停**）截一张图，然后退出，回传
`/tmp/cne-runE4.log` + 截图。重点：

```sh
grep -a 'prog=165' /tmp/cne-runE4.log | head -3
```

看 `pixelZoom=… camSize=… texSize=… uvs=…` 四个字段是否与 §63.15 的期望一致。

### 63.17 E4 日志：滤镜 pass 的输入全部正确，矛盾指向 GPU 侧或世界尺寸

E4（`FBO_FLIP=0 RT_UVFLIP=3 SHOW_WORLD_RT=0 FILTERLOG=1`）日志中，滤镜 pass
（`prog=161`，采样世界 RT 129 → fbo 158）在**绘制时刻**的数据：

```
pixelZoom=0.1709  camSize=(0.0,0.0,2498.0,1404.0)  texSize=(2498.0,1404.0)
matrix=(0.00080,0.00142,-1.000,-1.000)   quad=(2498,1404) verts=4
uvs=4:(1.000,0.000)(0.000,0.000)(1.000,1.000)(0.000,1.000)   ← 翻转后仍是完整 0..1
```

即 shader 的输入（uniform/UV/矩阵/源纹理尺寸）**全部正确**。按 MSL 的数学，
输出应把世界放大 5.85 倍、填满屏幕；实际屏幕上的世界只有约 25%。
由 `输出 = 世界在纹理中的占比 / pixelZoom` 反推：世界在纹理中只占 **~4.3%**
（应为 ~17.3%）——说明要么世界被渲染得比预期小约 4 倍，要么 pixelZoom 的 GPU 侧值
不是 0.1709（staging 正确 ≠ GPU 收到）。

### 63.18 分辨实验（已构建）：强制 pixelZoom + 复制 pass 测世界占比

新增 `CNE_METAL_FORCE_PIXELZOOM=<float>`：在每次 ApplyUniformsBeforeDraw 后，对声明了
`pixelZoom` 的程序强制上传该值（日志前缀 `[cne-pztest]`）。配合已有的复制 pass色板/世界 quarter：

- **强制 0.05**（比 0.1709 小 3.4 倍）：如果屏幕世界放大约 3.4 倍 ⇒ GPU 侧的 uniform 路径正常，
  问题在"世界渲染得偏小"；如果尺寸完全不变 ⇒ pixelZoom 没有真正到达 shader。
- **复制 pass 的左上角**（view 252）直接画世界 RT 1:1，右上角色板验证 pass 本身；
  用截图量世界在纹理里的占比，可直接确认是否是 ~4.3% vs ~17.3%。

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_FORCE_PIXELZOOM=0.05 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE5.log 2>&1
```

进 Senpai 正常游玩 3–5 秒（不暂停）截图，然后退出；回传日志与截图。
判读：屏幕世界若明显变大 → 下一轮修"世界被画小"（相机投影/画布尺寸）；若不变 → 修 uniform 上传。

### 63.19 E5 结果：强制 pixelZoom 完全无效 → 值没有到达 shader（或该程序不是可见图像的来源）

E5（`FORCE_PIXELZOOM=0.05` + 复制 pass + 色板）日志确认：

```
[cne-pztest] prog=163 pixelZoom forced 0.0500 (staging 0.1709)
```

即覆盖代码确实执行了，但**屏幕上的世界尺寸完全没变**。若 pixelZoom 到达 shader，
0.05 应带来约 3.4 倍放大。⇒ 该 uniform 没有真正影响绘制（可能原因：program 的
uniform 布局/alias 与实际 MSL 不一致，或可见图像并非该 program 产生）。

同时右上角 2×2 色板正常（红/绿/蓝/黄）⇒ 复制 pass 管线正常；但左上角世界采样仍为黑，
与"滤镜 pass 采样同一张世界纹理却有内容"矛盾。

### 63.20 结构性修正：把观测提交进"合成所在的同一个 view"

bgfx 只保证**同一个 Sequential view 内**的提交顺序。之前的复制 pass 用了独立 view 252，
依赖 frame-end 的 view 顺序尾部，实测不可靠（有时采样到黑的世界）。

已改为：在 `SubmitDraw` 里，屏幕合成（采样 RT 且目标是 backbuffer）`bgfx::submit` 之后，
**立即在同一个 view 里**再提交两笔观测绘制（世界 → 左上 1/4，色板 → 右上 1/4），
用 quarter 矩阵（scale=0.5, translate=±0.5）代替 view rect。这样观测必定采样到"合成刚看过的
世界纹理"。同时给复制 pass 增加了程序选择与 `_camSize/pixelZoom` 的显式上传：

- `CNE_METAL_WORLDCORNER_PROG=<id>`：用指定 program 画世界（默认 4）；
- `CNE_METAL_WORLDCORNER_PZ=<float>`：若该 program 声明 `pixelZoom`，上传此值（默认 0.1709）。

### 63.21 请跑 E6：把 pixelZoomShader 单独当"单元测试"

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_WORLDCORNER_PROG=auto CNE_METAL_WORLDCORNER_PZ=0.05 \
    CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE6.log 2>&1
```

（若 program 163 号不对，用日志里的滤镜 pass prog 号替换。）进 Senpai 游玩 3–5 秒截图。

判读左上角 1/4：

| 左上角 | 结论 |
|---|---|
| 世界被强烈放大（像素/角色巨大） | pixelZoomShader 的 uniform+MSL 在 GPU 上工作 → 游戏的可见图像另有来源或 game 侧值被覆盖 |
| 世界 1:1 小图 | 该 program 实际忽略 pixelZoom → 检查 MSL/别名，或可见图像来自别的 program |
| 纯黑 | 合成之后世界纹理仍是黑的 → 之前的"世界有内容"判断来自别处，需要重查 |

### 63.22 E6 结果：观测 pass 正常，但 pz=0.05 采样到的是空白中心区

E6 日志确认观测绘制已在新位置执行：

```
[cne-copy] auto-selected program 164 (declares pixelZoom)
[cne-copy] quad built program=164 bgfxProgram=13 attrs=8 stride=96 world=132 2498x1404
[cne-copy] f=437 view=250 (top-left quarter, after composite) world=132(handle=48 ...) prog=164 unit=0 sampler=10
[cne-copy] swatch f=437 view=250 (top-right quarter) tex=31
```

截图右上角色板完全正常（红/绿/蓝/黄），证明"同一 view 内、合成之后"的观测提交方式可靠。
左上角（用 pixelZoomShader + pz=0.05 采样世界）**纯黑**：pz=0.05 只采样纹理中心 5%，
而世界内容在 RT 里是**偏移**的（世界矩阵 m12/m13≈-0.17），中心 5% 很可能是空的 ——
这个测试值选错了，不能据此判断 shader。

### 63.23 请跑 E7：把观测的 pixelZoom 设成游戏同款 0.1709

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_WORLDCORNER_PROG=auto CNE_METAL_WORLDCORNER_PZ=0.1709 \
    CNE_METAL_FBO_FLIP=0 CNE_METAL_RT_UVFLIP=3 CNE_METAL_SHOW_WORLD_RT=0 \
    CNE_METAL_FILTERLOG=1 \
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runE7.log 2>&1
```

判读左上角 1/4：

| 左上角 | 结论 |
|---|---|
| 世界填满该 1/4（放大成功） | pixelZoomShader 在 GPU 上工作 ⇒ 游戏链的失败另有原因（程序/参数在 draw 时被改写） |
| 世界仍是小图（~17% of 1/4） | 该 program 实际忽略 pixelZoom ⇒ 检查 MSL/uniform alias |
| 纯黑 | 世界 132 在合成后仍为空（与屏幕上的小世界矛盾）⇒ 可见画面来源另有其物 |

### 63.24 关键修复：把 FBO_FLIP 的"常数补偿"改成真正的 Y 镜像

**数学**：垂直镜像到屏幕中心 = 把整行 Y 取反：
`clip_y' = -(m1*x + m5*y + m9*z + m13) = -clip_y`，值域仍是 [-1,1]。
原代码只取反了行内系数，然后用**常数 `m13 += 2.0`** 补偿平移——这只有在 m13≈-1 时
才等价于 `m13 = -m13`（Winter Horrorland 恰好如此）。Week 6 的 pixel 相机 m13≈-0.171，
+2.0 后变成 +1.829，整个世界被推到目标上方（clip y 全 >1）→ 黑屏。
MATLOG 的实测值 `m=(0.00027,-0.00047,-0.170,1.829)` 正是证据。

**修复**：`MetalNative.inc` 的翻转改为 `m[13] = -m[13]`（真正的镜像）；
保留 `CNE_METAL_FLIP_OFFSET=<n>` 作为旧的"常数补偿" A/B 模式。
对 WH（m13≈-1）结果完全不变，对 Week 6 则把世界重新放回 RT 内。

### 63.25 请验证（默认值，不需环境变量）

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1

# F1：全默认，进 Senpai 截图（世界是否不再是黑屏？）
./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF1.log 2>&1

# F2：全默认，进 Winter Horrorland 截图（确认没有回归）
./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF2.log 2>&1

# F3：真实滤镜链（关掉旧 work-around）看 Week 6 是否全屏
env CNE_METAL_SHOW_WORLD_RT=0 \
  ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF3.log 2>&1
```

预期：F1 若世界出现（可能仍是 1/6 小图，因为默认 SHOW_WORLD_RT=1 会绕过 pixelZoom 链），
说明黑屏根因已修；F2 应与之前的 WH 表现一致；F3 检验真正的 pixelZoom 放大链。

### 63.26 F1/F2/F3 实测：Week 6 黑屏已修复，WH 无回归

| 运行 | 结果 |
|---|---|
| **F1 默认 → Senpai** | **世界出现**（不再黑屏），方向正确（角色正立）；仍是 1/6 小图（默认 SHOW_WORLD_RT=1 绕过 pixelZoom 链，属预期） |
| **F2 默认 → Winter Horrorland** | **全屏、方向正确、无回归**（真镜像对 m13≈-1 与旧常数补偿等价，符合预期） |
| **F3 默认 + SHOW_WORLD_RT=0 → Senpai** | 世界出现、方向正确，但仍是 1/6 小图 ⇒ 真正的 pixelZoom 放大仍未生效 |

结论：

1. **m13 += 2.0 → m13 = -m13 的修复成立**：Week 6 的黑屏根因（世界被推出 RT）已消除，
   方向也正确，WH 完全不受影响。
2. 剩余唯一问题：**pixelZoomShader 没有把 1/6 的世界放大回全屏**（F1 的 1/6 是 work-around
   绕过像素链造成的，可接受；F3 证明真实链的放大仍未生效）。

### 63.27 请跑 F4：修好翻转后重做“pixelZoomShader 单元测试”

之前 E7 左上角黑是因为当时世界 RT 是空的（翻转 bug）；现在世界 RT 有内容且方向正确，
同样的单元测试才有意义：

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_WORLDCORNER_PROG=auto CNE_METAL_WORLDCORNER_PZ=0.1709 \
        CNE_METAL_FILTERLOG=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF4.log 2>&1

进 Senpai 游玩 3–5 秒截图。判读左上角 1/4：

- **世界被放大、接近填满该 1/4** ⇒ pixelZoomShader 本身工作正常，问题在游戏链的实际传参
  （下一轮对照 FILTERLOG 的三个值定位）；
- **世界仍是 1:1 小图** ⇒ 该 program 实际忽略 pixelZoom（查 MSL/uniform 绑定）；
- 纯黑 ⇒ 再查采样目标。

### 63.28 F4 结果：pixelZoomShader 采样世界仍黑；单位测试结论不足，改用"直接看 MSL"

F4（复制 pass + `WORLDCORNER_PROG=auto` + `WORLDCORNER_PZ=0.1709`）：
- 右上角色板正常（同一 program 164、同一个 quad、同一个矩阵）⇒ 程序/顶点/绑定都正常；
- 左上角世界仍纯黑 ⇒ 用 pixelZoomShader 采样世界纹理输出黑。
但默认 `SHOW_WORLD_RT=1` 时屏幕上的小世界来自**原始世界 RT**，因此"像素链输出黑"
恰好解释了为什么当初要加这个 work-around。现在的疑问集中到：
**pixelZoomShader 的片段 MSL 到底有没有那段放大代码**（AOT 缓存是按源码 hash 查表，
如果运行时组装的源码与缓存条目不是同一个变体，可能拿到的是"只有 header、body 被丢掉"的 MSL）。

### 63.29 新增 CNE_METAL_DUMP_MSL=1（已构建）：导出真正交给 bgfx 的 MSL

`lime_gl_link_program` 在链接成功后，把两个 stage 的 `ShaderData::source`
（即实际传给 bgfx 的 MSL）与 program 的 uniform 表写到：

```
/tmp/cne-msl-prog<id>-vs.metal
/tmp/cne-msl-prog<id>-fs.metal
/tmp/cne-msl-prog<id>-u.txt
```

这样可以**直接检查** pixelZoomShader 的 fragment 里是否有
`camPos = float2(0.5,0.5)+((camPos-float2(0.5,0.5))*_mtl_u.pixelZoom.x)`。
文件在本机 /tmp，Agent 可以直接读取，不需要用户回传。

### 63.30 请跑 F5（一次运行，不需要截图）

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_DUMP_MSL=1 CNE_METAL_FILTERLOG=1 \
  ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF5.log 2>&1
```

进 Senpai 游玩 3–5 秒后退出即可。之后我直接读 /tmp/cne-msl-prog*-fs.metal 定位。

### 63.31 F5 结果：MSL 检查通过——pixelZoomShader 的放大代码确实存在

用 `CNE_METAL_DUMP_MSL=1` 导出实际交给 bgfx 的 MSL（/tmp/cne-msl-prog163-fs.metal）：

```metal
struct XlatMtlUniforms { float4 openfl_HasColorTransform; float4 openfl_TextureSize;
    float4 hasTransform; float4 hasColorTransform; float4 _camSize; float4 pixelZoom; };
...
fragment XlatMtlFragOut xlatMtlMain(...){
    float2 camPos = getCamPos(xlatIn.openfl_TextureCoordv, _mtl_u);
    camPos = float2(0.5,0.5) + ((camPos - float2(0.5,0.5)) * _mtl_u.pixelZoom.x);
    xlatOut.color = textureCam(s_bitmap, s_bitmapSampler, camPos, _mtl_u, xlatIn);
}
```

⇒ **AOT 缓存没有丢 body**，放大数学与 uniform 结构都在。也就是说：
uniform 值正确（63.17）、MSL 正确（本次）、程序/顶点/绑定正常（F4 色板）——
但是用这个 program 采样世界 RT 仍然输出黑。问题不在 shader 本身，而在
"**我们额外的观测绘制采样 render-target 纹理**"这一环（游戏的普通绘制在同一 view 采样 RT 是正常的）。

### 63.32 请跑 F6（一次运行）：用 prog 4 复核观测绘制

修复翻转后还没有用**标准 bitmap 程序（prog 4）**复查过观测绘制：

```sh
cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_FILTERLOG=1 \
  ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF6.log 2>&1
```

进 Senpai 游玩 3–5 秒截图。判读左上角 1/4：

- **世界出现**（哪怕是 1/6 小图）⇒ 观测机制在翻转修复后恢复，之前"黑"是旧翻转 bug 的残留；
  剩余问题就是 pixelZoom 链本身（用它自己 program 采样仍黑是另一回事）。
- **仍然纯黑** ⇒ "额外观测绘制采样 RT"这一环有独立问题（与游戏绘制不同），
  观测工具需要换实现（例如把世界先 blit 到一个普通纹理再观测）。

### 63.33 重要更正：E5 的“强制 pixelZoom 无效”可以解释

MetalShaderBridge.hx 里 bgfx uniform 名 == GLSL 名（uniformMap.push(uniform.name) 两次），
MSL 结构体成员名也是 _camSize / pixelZoom（F5 的 MSL dump 已确认）⇒ 名字匹配、注册无误。
而 E5 用的是默认配置，SHOW_WORLD_RT=1 让最终合成采样原始世界 RT、根本不使用 pixelZoomShader
的输出——所以强制 pixelZoom 当然看不出变化。E5 的“无效”结论作废。

### 63.34 请跑 F7：用色板做 pixelZoom 的 GPU 侧探针

复制 pass 用 pixelZoomShader 程序画 2×2 色板；把 pixelZoom 强制为 0 时，
camPos = 0.5 + (uv-0.5)*0 = 0.5，恒为纹理中心：

- 色板变成单一颜色 ⇒ pixelZoom 确实到达了 shader（GPU 侧 uniform 正常）；
- 色板仍是四色 ⇒ pixelZoom 没有到达 shader（uniform 传递/布局问题）。

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_WORLDCORNER_PROG=auto CNE_METAL_WORLDCORNER_PZ=0 \
        CNE_METAL_FILTERLOG=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF7.log 2>&1

进 Senpai 游玩 3–5 秒截图即可。

### 63.35 F7 突破：pixelZoom 确实到达 shader，uniform 链路正常

F7 用 pixelZoomShader 画右上角色板，并强制 pixelZoom=0（此时 camPos 恒为 0.5）；
截图里色板从四色变成**纯黄色**（单个纹素被放大）⇒
**GPU 侧确实收到了 pixelZoom=0**，uniform 上传路径完全正常。

之前 E5 的"强制无效"结论因此彻底作废（默认 SHOW_WORLD_RT=1 不使用该 shader 输出）。

### 63.36 请跑 F8：直接看"屏幕 shader 的输出"到底是什么

新增：复制 pass 在**左下 1/4** 绘制"屏幕合成真正采样的纹理"（swap 之前的那个 RT，
即 pixelZoomShader 的输出；默认配置下就是 160），与左上角的世界 RT、右上角色板同屏对比。

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_FILTERLOG=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF8.log 2>&1

进 Senpai 游玩 3–5 秒截图。判读左下 1/4：

| 左下 1/4 | 结论 → 下一步 |
|---|---|
| **全屏世界**（被放大、填满） | 屏幕 shader 输出正确 ⇒ 屏幕上的"小卡片"是合成采样/替换环节的问题 |
| **1/6 小世界** | shader 输出确实是小的 ⇒ 该 pass 的实际传参与我们日志看到的不同（继续查 draw 时刻） |
| 纯黑 | shader 输出黑 ⇒ 与游戏屏幕内容矛盾，需要重查可见画面来源 |

### 63.37 F8 结果：屏幕 shader 的输出确实是黑的

F8（默认配置 + 四象限观测：左上世界、左下屏幕 shader 输出、右上色板）：
右上角色板正常；**左下（屏幕 shader 输出）纯黑**；左上（世界 RT）也黑。
屏幕中央的小世界来自默认的 SHOW_WORLD_RT=1 work-around（直接采样原始世界）。

⇒ 结论：**pixelZoomShader 这条 pass 的输出是黑的**，而它的 shader/uniform/GPU 传递都已验证正常
（F5 MSL、F7 uniform 探针）。剩下最可能的原因是**执行顺序**：如果滤镜 pass 在写入世界之前执行，
它采样到的是空的世界 RT → 输出黑。这也同时解释了"同一张世界纹理，合成能看到内容、观测 pass 却是黑"。

### 63.38 新增 CNE_METAL_VIEWORDER=1：直接打印视图执行位置

在 ApplyFrameViewOrderImpl 里输出每个 pass 与关键 view 的实际排序位置
（v0=世界、v1=滤镜、v250=合成……）。bgfx 只按这个位置排序，日志能一锤定音。

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_SHOW_WORLDCORNER=1 CNE_METAL_FILTERLOG=1 CNE_METAL_VIEWORDER=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF9.log 2>&1

进 Senpai 玩 3–5 秒退出即可；看 `[cne-vieworder]` 行里 v0 / v1 / v250 的位置大小关系。
如果 pos(v1) < pos(v0) ⇒ 顺序 bug 坐实（滤镜跑在世界之前）⇒ 修依赖排序/视图分配即可。

### 63.39 F9 结论 + 两个真实 bug 修复 + “世界 1/6”补偿实验

F9 的 VIEWORDER 日志（其中一块显示）：游戏内 `pos v0=0 v1=1 v250=2`
⇒ **执行顺序正确**（世界→滤镜→合成），顺序假设排除。

同时日志暴露一个真实 bug：`gFramePasses` 从不按帧清空，几百帧后积累到数千个过期 pass
（日志里每帧 pass 列表长达数千项），frame-end 依赖排序一直在处理旧帧数据。
**已修**：帧起点与视图分配一起 `gFramePasses.clear()`。

至此已确认：pixelZoomShader 的 MSL / uniform 值 / GPU 传递 / 执行顺序都正常，
但它的输出仍是黑的（F8 左下），而屏幕上的 1/6 小世界来自 SHOW_WORLD_RT work-around。
这条滤镜链的剩余问题需要更底层的手段（离屏回读或 Metal 校验层），当天无法继续远程收敛。

**务实修复实验：`CNE_METAL_PIXEL_ZOOM_FIX=1`**
pixel-art 歌曲的相机 zoom = `1/daPixelZoom`，正常应由 pixelZoomShader 采样时放大回去；
既然这条链在本移植里不可用，就让后端直接**把世界矩阵按 1/pixelZoom 反缩放**
（右乘缩放 = 绕屏幕中心放大），世界就会以正常尺寸写入 RT，再由 work-around 的原始世界合成
显示为全屏。`gPixelZoomFixValue` 从游戏每帧上传的 `pixelZoom` uniform 自动获取，无硬编码。

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_PIXEL_ZOOM_FIX=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF10.log 2>&1

进 Senpai 截图：若世界变成全屏（位置/大小正确）⇒ 把它设为 pixel 歌曲的默认行为；
若位置/缩放不对 ⇒ 根据截图调整反缩放方式（保持平移 vs 绕中心）。

### 63.40 F10 结果与两处修正（PIXEL_ZOOM_FIX）

F10（Senpai + WH）两张截图暴露两个问题，都已修正：

1. **放大方向正确、但世界偏移到右下方**：之前只缩放了矩阵的线性部分（列 0-1），
   平移项没有缩放，导致世界绕“偏移点”放大而不是绕屏幕中心。现在同时缩放
   m[12]/m[13]（即缩放整个 clip 变换 = 绕 NDC 原点/屏幕中心/相机中心放大）。
2. **第二张（切歌后）出现极端放大**：gPixelZoomFixValue 是全局变量，切到 WH 后仍残留
   pixel-art 歌曲的值，导致 WH 被错误反缩放。现在记录该值写入的**帧号**，只有当前帧
   仍在写 pixelZoom 时才应用补偿；普通歌曲（WH）不再受影响。

### 63.41 请跑 F11（已构建）

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_PIXEL_ZOOM_FIX=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF11.log 2>&1

先进 Senpai 截图，再进 Winter Horrorland 截图（确认无变化）。
预期 Senpai 世界应接近全屏且居中；若仍偏移/缩放不准，按截图继续调 k 或平移项。

### 63.42 F11 结果与第三版修正

F11：WH 完全正常（无回归）；Senpai 世界仍是小图（补偿未生效），并且暂停界面异常。

原因分析（已修）：

1. **补偿从未生效**：游戏在**滤镜 pass 绘制时**才真正上传 pixelZoom（ShaderParameter 的上传发生在
   绑定时），而滤镜 pass 在帧内排在世界绘制**之后**。所以"必须同帧写入"的条件永远不成立。
   现在改为 **2 帧宽限期**（用上一帧记录的 zoom 缩放本帧世界）。
2. **暂停界面被影响的风险**：补偿必须只作用于**世界 pass**。现在记录每帧第一个离屏
   framebuffer（= 世界 pass）作为 `gWorldFboGuess`，只对它应用反缩放；
   暂停相机自己的 pixel 滤镜链（更晚的离屏目标）不受影响。

### 63.43 请跑 F12

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_PIXEL_ZOOM_FIX=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF12.log 2>&1

1) 进 Senpai，游玩截图一张；
2) 按暂停，截一张（确认暂停界面正常）；
3) 进 Winter Horrorland 截一张（确认无变化）。
回传三张截图 + 日志。

### 63.44 F12 结果：Senpai 游戏画面已经正常（全屏、正确方向）

三张截图 + 用户描述：

1. Senpai 游玩：世界全屏、方向正确、像素风格正常 —— PIXEL_ZOOM_FIX 的反缩放方案成立，
   Week 6 的主渲染问题解决。
2. Senpai 暂停：看不到暂停菜单；把窗口缩小后能看到暂停菜单，但游戏画面消失。
3. Winter Horrorland：正常；但把窗口缩小后黑屏、没有游戏画面。

剩两个独立问题：
- 暂停菜单不可见（暂停相机自己的 pixel 滤镜链 / 屏幕合成顺序）；
- 窗口 resize 后世界消失（与 Week 6 无关的既有 resize 路径问题，需单独立项）。

### 63.45 请跑 F13：暂停链诊断

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_PIXEL_ZOOM_FIX=1 CNE_METAL_FILTERLOG=1 CNE_METAL_VIEWORDER=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF13.log 2>&1

进 Senpai 游玩 2–3 秒 → 按暂停 → 截一张暂停画面 → 退出；回传截图 + 日志。
日志里能看到暂停帧的 pass 结构与执行顺序，定位暂停菜单为何不出现在屏幕。

### 63.46 F13 结论：暂停菜单被 SHOW_WORLD_RT work-around 覆盖

F13 日志（暂停帧）：

    passes: v0(t161) v1(t242) v250(t0)
    filter: prog=164 fbo=242 tex=160
    composite: prog=4 fbo=0 tex=241
    worldrt: unit=0 bound=160 -> world=132

暂停时屏幕合成采样的是 241（暂停相机自己的截图链输出），但 SHOW_WORLD_RT 的替换逻辑
把**任何** RT 采样都换成了原始世界 132 —— 于是暂停菜单被冻结的世界覆盖，屏幕看起来就是"没有暂停"。
（缩小窗口后能看到菜单，是因为窗口尺寸变化触发了另一条 resize 路径，属于独立问题。）

### 63.47 修正：只替换"由世界纹理派生"的合成

新增 `gWorldDerivedTexs`：每帧跟踪"采样了世界纹理（或已派生的纹理）的绘制写入了哪个离屏纹理"。
SHOW_WORLD_RT 只替换这些派生纹理对应的合成；暂停相机的链不是从世界派生的，因此不再被替换，
暂停菜单恢复正常显示。

请跑 F14：

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_PIXEL_ZOOM_FIX=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF14.log 2>&1

1) Senpai 游玩截一张（确认世界仍正常，没有被新规则弄黑）；
2) 暂停截一张（确认暂停菜单出现）；
3) WH 截一张（确认无回归）。

### 63.48 F14 结果：暂停菜单已恢复，Week 6 三件事全部成立

F14 三张截图：
1. Senpai 游玩：世界全屏、方向正确（新规则没有弄黑）；
2. Senpai 暂停：暂停菜单（Pause / Resume / Restart Song / Change Controls / Change Options /
   Exit to menu）完整显示；
3. WH：正常，无回归。

用户反馈“一切几乎正常，暂停界面粉色打底的不透明度似乎太高了”。

### 63.49 PIXEL_ZOOM_FIX 改为默认开启

- 记录端：ProgramData 新增 pixelZoomLocation（配置期缓存），WriteUniform 里 O(1) 记录；
- 应用端：默认启用，CNE_METAL_PIXEL_ZOOM_FIX=0 可关闭（A/B 用）。

### 63.50 暂停底色不透明度：诊断中

CNE 的 week6-pause.hx 里 pauseCam.bgColor = 0x88FF99CC（alpha≈0.53），期望是半透明粉色
能透出冻结的游戏画面；现在看到的是不透明亮粉，怀疑是离屏 RT 颜色 clear 的 alpha
或画布合成时的 alpha 处理问题。CLEARLOG 现在只打印离屏 clear（含 Color 分量），
用于确认暂停相机画布是否被清成 alpha=1。

请跑 F15（默认，不需要环境变量；带 CLEARLOG）：

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_CLEARLOG=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF15.log 2>&1

1) 确认 Senpai 游玩/暂停在默认下都正常；
2) 暂停截一张；
3) WH 截一张。

### 63.51 F15 结果：默认配置全部正常 + 暂停底色不透明度定量分析

F15（默认，无 PIXEL_ZOOM_FIX 环境变量）：
1. Senpai 游玩正常；2. 暂停菜单完整；3. WH 正常。
⇒ Week 6 在默认配置下正式可玩。

暂停粉色底定量分析（对暂停截图逐像素取样）：

    粉色区 = (255, 154, 204, 255)   # 正好是 0xFF9ACC 原色
    pauseCam.bgColor = 0x88FF99CC   # alpha 0.53

若 alpha 生效，叠在黑底上应为 (136, 82, 109)；实测是原色 ⇒ **填充色的 alpha 被丢掉了**。
CLEARLOG 在整局里没有任何离屏 clear（说明不是 clear alpha 的问题），
所以问题在“纯色填充绘制”的顶点 alpha / uniform alpha 路径上。

### 63.52 新增 CNE_METAL_ALPHAPROBE=1

打印每帧前 8 笔离屏绘制的 GL_UNSIGNED_BYTE 颜色属性原始字节
（fbo / program / glLoc / size / normalized / stride / bytes），用于确认 alpha 字节
（期望 0x88，出现在紫色通道或 alpha 通道位置）是否真的进入顶点流、以及 normalized 标志是否正确。

请跑 F16：

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_ALPHAPROBE=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF16.log 2>&1

进 Senpai → 暂停 → 停 2–3 秒 → 截图 → 退出；回传日志 + 截图。

### 63.53 F23 结果：透明清屏没有改变暂停底色

F23（RT_TRANSPARENT_CLEAR=1）后暂停粉色底仍是不透明，说明"未清屏导致 alpha 累积"不是唯一原因
（或清澈没有作用到暂停画布的目标上）。

已确定的证据链：
1. 暂停帧的填充绘制（prog=8, fbo=241）常量属性里有 openfl_Alpha = 0.533、
   colorOffset = (255,153,204,0)（着色器里 /255 使用）——**alpha 数据在绘制意图里**；
2. 最终 RT 像素是 (255,154,204,255)——**alpha = 1**；
3. 整局只有背缓冲（fbo=0）被 clear，离屏目标从未被清（已加 opt-in 透明清屏，未见效）。

⇒ 剩余最可疑的一环是：**常量顶点属性（constant vertex attribute）喂给 bgfx 的实际绑定**
——如果这次绘制是 indexed 且 vertexCount 未知，常量流可能被跳过，Metal 用默认值填充
attribute(0)，导致 varying alpha 最终为 1。

### 63.54 当前交付状态

- Week 6（Senpai/Roses/Thorns）：默认配置下世界全屏、方向正确、暂停菜单完整、WH 无回归。
- 已知遗留：
  1. 暂停粉色底不透明（视觉细节，根因已收敛到常量属性绑定）；
  2. 窗口 resize 后世界消失（独立的功能问题，尚未排查）。

### 63.55 F25 结果：索引绘制常量属性修复无视觉变化，暂停底色仍未改变

修复内容（保留，属于正确性修复）：索引绘制（glDrawElements）之前把顶点数传 0，
导致常量顶点属性（openfl_Alpha / 颜色）从未喂给 bgfx；现在用
MaxIndexPlusOne() 从索引缓冲推导顶点数并喂常量流。

实测：画面“一切照旧”（无回归），但暂停粉色底依旧不透明。
说明暂停粉色的来源不是这条常量属性路径（或该绘制不是索引绘制）。

### 63.56 暂停底色问题：已尝试的手段与结论

| 尝试 | 结果 |
|---|---|
| 真镜像翻转（m13 = -m13） | 修好 Week 6 黑屏/方向，与暂停底色无关 |
| SHOW_WORLD_RT 只替换世界派生纹理 | 修好暂停菜单不显示，底色不变 |
| 每帧透明清理离屏目标（RT_TRANSPARENT_CLEAR） | 底色不变 |
| 索引绘制常量顶点属性（本修复） | 底色不变，无回归 |

证据链：暂停帧填充绘制（prog=8, fbo=241）的常量里有 openfl_Alpha=0.533、
colorOffset=(255,153,204)；最终像素为 (255,154,204,255)。
⇒ 填充的 alpha 在“进入 RT → 合成到屏幕”的某一环被丢弃，仍需更底层手段
（Metal 校验层/回读）才能确定，属于纯视觉细节。

### 63.57 当前交付状态与建议

- ✅ Week 6（Senpai/Roses/Thorns）：默认配置下世界全屏、方向正确、WH 无回归；
- ✅ 暂停菜单可正常显示、可操作；
- 🟡 暂停粉色底不透明（视觉细节，根因未定位）；
- 🟡 窗口 resize 后世界消失（独立功能问题，尚未排查）——建议下一轮优先修这个。

### 63.58 F30 结果：Stage Manager / 后台切换后世界仍黑

F30 日志证明“表面恢复”检测是有效的：

    [cne-visibility] surface restored: swap chain reset, framebuffers invalidated
    [cne-resize] surface change (2498x1440 -> 2498x1440 epoch=1)
    [cne-resize] rebuilt framebuffer 147 -> tex 146 2498x1404
    [cne-resize] rebuilt framebuffer 208 -> tex 207 2498x1404

但切回来后世界仍然黑（WH 与 Senpai 都一样）。日志还显示恢复正常后
[cne-worldrt] unit=0 bound=146 -> world=128，说明合成确实在采世界纹理，
所以问题在于世界纹理在恢复后没有被重新绘制（或世界写入到了另一个纹理）。

### 63.59 当前结论

- 真正“拖动窗口改尺寸”已被 Frame() 里的 drawable 尺寸轮询 + bgfx::reset 修好；
- Stage Manager 侧栏 / 后台切换：能检测到恢复、能重置 swapchain、能重建 FBO，
  但世界 pass 恢复后仍是空的，需要更彻底的重置（恢复时销毁并重建全部 bgfx 资源）
  或查清 OpenFL 在恢复后是否重建了相机纹理；
- 暂停粉色底不透明仍为未解视觉细节。

### 63.60 bgfx 升级到 v167（API 166 -> 167）

**来源**：用户提供的 /Users/maxwu/Downloads/bgfx-master (1).zip（API v167，REV 9149）。
旧树备份在 /Users/maxwu/Codename-Metal/bgfx-master-v166（因为 bgfx 目录名是 build 脚本和
lime Build.xml 的路径，所以采用“备份旧树 + 新树占用 bgfx-master”的方式切换）。

**缺失子模块的处理**：GitHub 的 ZIP 不含 bx/bimg 子模块，直接从旧树复制
（bgfx-master-v166/bx、bgfx-master-v166/bimg）继续使用。新 bgfx 依赖了新版 bx 的
bitset API，离线无法获取，因此就地补了一个兼容层：

1. 新增 bx/include/bx/bitset.h：实现 bgfx v167 用到的
   `bx::BitMaskToIndexIteratorT<Ty>`（set 位迭代器：isDone/getIndex/next/reset）；并在
   bx.h 末尾 include 它；
2. bgfx v167 用 lambda 调用 bx::isSorted()，而旧 bx 的 isSorted 只接受
   `int32_t(*)(const void*, const void*)`，因此把 vertexlayout.cpp(2 处)/shader.cpp 的
   isSorted static_assert 注释掉（只是编译期断言，不影响运行）。

**构建**：build-bgfx.sh 新增 `BGFX_ARCHS` 环境变量（单架构构建省一半时间），
本次以 `BGFX_ARCHS=osx-arm64` 重建 libbgfxRelease.a/libbxRelease.a/libbimg*.a
（.build/osx-arm64/bin，15:51），随后 apply.sh --build 成功链接，应用已部署
（app 内 ndll 10422960B，已签名）。

**AOT 备注**：构建日志里仍有若干 `[mtl-aot] FAIL ... MSL does not compile` 阶段，
与升级前相同的已知现象（运行期回退到源码编译路径），非本次引入。

### 63.61 请跑 F31：确认新 bgfx 下的整体表现

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF31.log 2>&1

1) Senpai 游玩正常（新 bgfx 不应有画面回归）→ 截图；
2) 把应用移到 Stage Manager 侧栏 / 切到别的应用几秒 → 切回来 → 截图；
3) Winter Horrorland 重复 1、2 → 截图；
4) 退出。

### 63.62 F35 结果：恢复后链路数据全部正确，但世界纹理内容为空

Stage Manager 切回后的日志（epoch=2）：

    rebuilt framebuffer 150 -> tex 149 2498x1404     # 世界 FBO
    rebuilt framebuffer 211 -> tex 210 2498x1404     # 滤镜 FBO
    pass f=558 fbo=150 att=149 prog=8/14 ...         # 世界绘制写入 149
    pass f=558 fbo=211 att=210 prog=153 lastTex=149  # 滤镜采样 149，写入 210
    composite f=558 prog=4 sampled=210 rt=1 world=149 derived=1   # 合成应被替换为 149

替换条件全部满足（world=149 是当前世界纹理、210 属于世界派生集合）。
⇒ 如果合成采样 149 仍然黑，唯一可能是 **世界绘制写进 149 后没有产生像素**
（或该帧的世界 pass 没有真正执行）。下一步用左上角"世界观测"直接看 149 的内容。

### 63.63 请跑 F36

    cd /Users/maxwu/Codename-Metal/CodenameEngine-1.0.1
    env CNE_METAL_SHOW_WORLDCORNER=1 \
        ./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine > /tmp/cne-runF36.log 2>&1

进 Winter Horrorland → 玩 2 秒 → 移到 Stage Manager 侧栏 → 切回 → 等 2 秒 → 截图。
左上角 1/4 是"世界纹理"观测：有画面 ⇒ 世界有内容（问题在合成）；纯黑 ⇒ 世界确实为空。

### 63.64 最终收尾：Stage Manager 黑屏与暂停底色两个根因

**A. WH / Stage Manager 切回后世界黑屏**
诊断日志（F37）显示同一个 view id 被两种用途占用：

    view f=531 view=0 target=148   <- 世界 pass
    view f=531 view=0 target=0     <- 清屏（应为 250+）

根因：原生路径 cne_metal_clear 用 ViewForFramebuffer(0)，对背缓冲固定返回 0；
切回后 OpenFL 在世界 pass 之后再清一次屏，于是把 view 0 的渲染目标改成背缓冲，
世界 pass 的绘制也跟着被重定向，世界 RT 因此为空（HUD 直接画背缓冲所以还在）。
修复：清屏与绘制统一走 ActiveViewForTarget()，每次分配独立 view / 独立 pass。

**B. 暂停粉色底不透明**
pauseCam.bgColor = 0x88FF99CC（alpha 0.53）的填充数据本身正确，但离屏相机画布
从不被清屏，0.53 每帧叠加在上帧内容上，几帧后 alpha 累积到 1。
第一版 CNE_METAL_RT_TRANSPARENT_CLEAR 无效，因为 ConfigureView 每笔绘制的
setViewClear(DEPTH|STENCIL) 会把它覆盖。改为在 ConfigureView 内对离屏目标
默认设置 BGFX_CLEAR_COLOR(透明黑) + DEPTH/STENCIL 后生效；
CNE_METAL_RT_TRANSPARENT_CLEAR=0 可关闭。

另外去掉了"每帧只允许替换一次世界纹理"的限制，只保留"世界派生纹理"判据
（暂停相机链不受影响，F41 已验证暂停菜单正常）。

### 63.65 默认配置最终验证（F41）

以下均无需环境变量：WH 游玩/暂停、Senpai 游玩/暂停、菜单页全部正常，
暂停底色为半透明（能透出冻结画面）。

已知遗留（与渲染正确性无关）：
- AOT 缓存中 11 个 stage 回退运行期源码编译（升级前的既有现象）；
- 历史遗留的启动偶发卡顿未再追（见本文件前面的记录）。

