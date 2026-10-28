# Codename Engine — Metal Backend (macOS)

[中文](#codename-engine--metal-backend-macos) · **Codename Engine 1.0.1** + a native **Metal / bgfx** rendering
backend for macOS. The game is a Friday Night Funkin' engine; this repository adds the port that makes it run
on Metal instead of OpenGL, AOT-compiles every GLSL shader to Metal Shading Language, and documents how it was
built and debugged.

> 本项目 = 上游 [Codename Engine 1.0.1](https://github.com/CodenameCrew/CodenameEngine) + 自研
> **Metal (bgfx) 渲染后端**。目标是在 macOS 上完全用 Metal 渲染（不再有 GL context），
> 并保持原版 OpenGL 路径的兼容性。

> **声明 / AI Disclosure**
> 本仓库中的 Metal (bgfx) 渲染后端移植、全部配套脚本、调试过程与文档修改，均由 **DeepSeek-V4.1-Flash** 生成。
> 上游 Codename Engine 代码、Lime/OpenFL/HaxeFlixel 生态及 bgfx 等第三方依赖的版权归各自作者所有，
> 遵循其原有许可证（见本文末尾 Credits & License）。


---

## 1. 特性概览

- **Metal 渲染**：所有 2D 渲染经 `lime_gl_*` GL 形状 API 仿真层 → `bgfx` Metal 后端提交；
  进程内没有 GL context，也没有 GL 交换链。
- **着色器 AOT**：构建期把 GLSL 翻译成 Metal Shading Language 并预编译为 `.metallib`
  （当前 116 个 stage 中 111 个带预编译库；运行时零 GLSL 翻译）。
- **双路径**：Lime 的 `lime_gl_*` CFFI 兼容层 + 更快的原生 `cne_metal_*` 路径（默认启用）。
- **可诊断**：`CNE_METAL_PROFILE`、`CNE_METAL_DRAWPROBE`、`CNE_METAL_COVERAGE`、
  `CNE_METAL_HITCH` 等 70 个环境变量开关（见 §8）。
- **已实机验证**：主菜单 / 警告页 / 开场视频 / 选谱 / Dusttale 战斗 / Winter Horrorland /
  暂停菜单 / 各 mod shader 特效。

---

## 2. 目录结构

```
Codename-Metal/
├── README.MD                  ← 本文件
├── LICENSE                    ← Apache-2.0（上游许可证副本）
├── docs/
│   └── METAL-TODO.md          ← Metal 后端剩余工作清单（架构/AOT/2D 功能/3D/工程债）
├── CodenameEngine-1.0.1/      ← 游戏工程（上游 CNE 1.0.1 源码 + 本仓库的 Metal 改动）
│   ├── project.xml
│   ├── source/                ← Haxe 游戏源码（含少量 Metal 相关改动）
│   ├── metal/                 ← ★ 本仓库核心：Metal/bgfx 后端
│   │   ├── native/            ← C++ 后端（lime_gl_* 全部 271 个入口 + cne_metal_* 原生路径）
│   │   ├── haxe/              ← GLSL→MSL 翻译器、ShaderAot 缓存、Metal 相关 Haxe shim
│   │   ├── scripts/           ← 构建/安装/检查脚本（apply.sh、build-bgfx.sh、gen_shader_aot.py…）
│   │   ├── tools/             ← .metallib 构建工具、直连 Metal 回放原型等
│   │   ├── audit/             ← 渲染路径/原语/着色器 AOT 审计文档
│   │   └── README.md          ← 详细开发日志（5000+ 行，按轮次记录）
│   ├── assets/、art/           ← 引擎自带资源（可选，见 §10）
│   └── export/                ← 构建输出（被 .gitignore 忽略）
├── bgfx-master/               ← bgfx 源码依赖（不纳入 Git，见 §5）
├── bx → bgfx-master/bx        ← bgfx 子模块符号链接（构建脚本自动创建）
├── bimg → bgfx-master/bimg
└── _local/                    ← 本机缓存/备份/下载（被 .gitignore 忽略）
```

---

## 3. 运行/构建状态（验证基线）

- 系统：macOS 26.6.2 (Apple Silicon / M3)，`MACOSX_DEPLOYMENT_TARGET=11.0`
- Haxe **4.3.7**，hxcpp **4.3.2**
- Lime **8.3.2**（dev 检出）+ 本仓库补丁（见 §5.3）
- bgfx API **v167**（Metal 后端，静态库）
- 通过：主菜单、Cutscene、选谱、Dusttale 全流程、Winter Horrorland、暂停菜单；
  运行时 `newLibraryWithSource=0`、`sourceCompile=0`（着色器全部走 metallib）。

---

## 4. Reference（上游与依赖）

| 项目 | 用途 | 链接 |
| --- | --- | --- |
| Codename Engine | 上游游戏引擎（本仓库从 1.0.1 派生） | https://github.com/CodenameCrew/CodenameEngine |
| bgfx | 跨平台渲染库；本后端使用其 **Metal** renderer | https://github.com/bkaradzic/bgfx |
| bx | bgfx 基础库（构建依赖） | https://github.com/bkaradzic/bx |
| bimg | bgfx 图像库（构建依赖） | https://github.com/bkaradzic/bimg |
| Lime | Haxe 原生窗口/资源层（被本后端 patch） | https://github.com/openfl/lime |
| OpenFL | 显示列表/渲染 API 层 | https://github.com/openfl/openfl |
| Haxe | 语言与编译器 | https://haxe.org |
| hxcpp | Haxe→C++ 运行时 | https://github.com/HaxeFoundation/hxcpp |
| HaxeFlixel | 游戏框架 | https://github.com/HaxeFlixel/flixel |
| hxvlc | 视频过场（VLC） | https://github.com/MAJigsaw77/hxvlc |
| flxanimate / hscript-improved / funkin-modchart … | 见 `CodenameEngine-1.0.1/project.xml` | — |
| Apple Metal | MSL / `MTLLibrary` / `CAMetalLayer` | https://developer.apple.com/metal/ |

本项目关键自研实现参考：

- `CodenameEngine-1.0.1/metal/native/MetalBindings.cpp`：`lime_gl_*` 全量实现（269+2 个入口）
- `CodenameEngine-1.0.1/metal/haxe/lime/graphics/metal/GLSLToMSL.hx`：GLSL→MSL 翻译器
- `CodenameEngine-1.0.1/metal/audit/`：原语/路径/AOT 三份审计（含数据与复现命令）
- `CodenameEngine-1.0.1/metal/README.md`：完整开发日志（问题 → 证据 → 修复）

---

## 5. 依赖安装

### 5.1 系统

- macOS 11+（Apple Silicon 或 Intel；当前脚本默认 `osx-arm64`）
- Xcode **Command Line Tools**（clang、metal 编译工具链；`xcode-select --install`）
- Python 3（AOT/metalib 生成脚本）
- `git`、`make`、`pkg-config`

### 5.2 Haxe 工具链

```sh
# Haxe 4.3.7 + hxcpp 4.3.2
haxelib install hxcpp 4.3.2

# 游戏库（project.xml 声明的全部依赖）
cd CodenameEngine-1.0.1
haxelib install flixel flixel-addons flxanimate format hscript-improved markdown away3d
haxelib install hxvlc hxdiscord_rpc nape-haxe4 funkin-modchart hxcpp-debug-server   # 可选/按需
```

> 若使用工程内的本地 haxelib 仓库（`.haxelib/`，默认被 git 忽略），可把它加入 haxelib 搜索路径：
> `haxelib setup` 里追加工程 `.haxelib` 目录，或把库直接 `haxelib install` 到默认仓库。

### 5.3 Lime 8.3.2（dev 检出）+ 补丁

本后端需要修改 Lime 的构建文件、GL 分发器与 Haxe 渲染上下文（14 个补丁点，幂等、锚点漂移会报错）：

```sh
git clone --depth 1 --branch 8.3.2 https://github.com/openfl/lime.git ~/haxelib-dev/lime-832
haxelib dev lime ~/haxelib-dev/lime-832
```

补丁由 `metal/scripts/apply.sh` 自动应用（见 §6），无需手工编辑。

### 5.4 bgfx（Metal 静态库）

> bgfx 官网/源码仓库：**https://github.com/bkaradzic/bgfx**（本后端构建的静态库即来自该仓库的 Metal renderer）。

```sh
# 1) 获取 bgfx 源码到工程同级目录（目录名必须是 bgfx-master）
git clone --depth 1 https://github.com/bkaradzic/bgfx.git ../bgfx-master
# bx / bimg 子模块会被 build-bgfx.sh 自动 clone

# 2) 构建 osx-arm64 静态库（约 2–5 分钟）
cd CodenameEngine-1.0.1
./metal/scripts/build-bgfx.sh
# 产物：~/bgfx-build/bgfx/.build/osx-arm64/bin/libbgfxRelease.a (+ bx/bimg)
```

> 若要 Intel 构建：`BGFX_ARCHS=osx-x64 ./metal/scripts/build-bgfx.sh`。

---

## 6. 构建

所有命令均在 `CodenameEngine-1.0.1/` 下执行。

### 6.1 首次完整构建

```sh
cd CodenameEngine-1.0.1

# (1) 安装 Metal 后端到 Lime 检出 + 生成 AOT 缓存（GLSL→MSL→.metallib）+ 重建 lime cpp ndll
export MACOSX_DEPLOYMENT_TARGET=11.0
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --build

# (2) 编译游戏（Haxe → cpp），生成 export/release/macos/bin/CodenameEngine.app
haxelib run lime build mac

# (3) 关键：把 cpp 版 ndll 装回 app（上一步会把 neko CLI bootstrap 放进 bundle）
./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --build --deploy
```

运行：

```sh
./export/release/macos/bin/CodenameEngine.app/Contents/MacOS/CodenameEngine
```

### 6.2 日常增量构建

| 改动位置 | 重新执行 |
| --- | --- |
| `metal/native/*`（C++） | `./metal/scripts/apply.sh --lime "$HOME/haxelib-dev/lime-832" --build --deploy` |
| `metal/haxe/*` 或 `source/*`（Haxe） | `haxelib run lime build mac` 然后 `./metal/scripts/apply.sh … --build --deploy` |
| 只改游戏资源 / mod | `haxelib run lime build mac`（或直接改 app bundle 内资源） |

### 6.3 AOT 着色器缓存

`apply.sh` 会在构建前自动调用 `metal/scripts/gen_shader_aot.py`：

- 枚举 OpenFL/Flixel/CNE/mod 的全部 GLSL → 翻译成 MSL → 编译 `.metallib`；
- 生成 `metal/haxe/lime/graphics/metal/ShaderAotCache.hx`（约 2.0 MB，含 111 个预编译库）；
- 修改翻译器或 shader 后如未生效，删除缓存强制重建：

```sh
rm -f metal/haxe/lime/graphics/metal/ShaderAotCache.hx
python3 metal/scripts/gen_shader_aot.py
# 删掉 hxcpp 的旧目标文件后再构建（hxcpp 不跟踪头文件/生成文件依赖）
rm -rf export/release/macos/obj/src/lime/graphics/metal/
```

---

## 7. 编译检查（不跑游戏）

```sh
./metal/scripts/check-compile.sh \
  --lime "$HOME/haxelib-dev/lime-832" \
  --hxcpp "$HOME/haxelib/hxcpp/4,3,2" \
  --out /tmp/obj-check
```

输出应为 `OK - Metal backend compiles against bgfx API v167 and Lime`。

---

## 8. 诊断开关（环境变量）

| 开关 | 作用 |
| --- | --- |
| `CNE_METAL_PROFILE=1` | 每 240 帧打印 CPU 前端耗时：draws/frame、bind、bgfx submit、uniform、纹理上传（µs/帧、MB/帧） |
| `CNE_METAL_HITCH=1` / `CNE_METAL_HITCH_DETAIL=1` | 报告 >34ms 的卡顿帧；DETAIL 拆分 game/submit/present/rest |
| `CNE_METAL_DRAWPROBE=1` | 逐 draw：program/fbo/tex/size/rt/m0/m5/m13 |
| `CNE_METAL_UVLOG=1` | 屏幕合成翻转判定（comp#/needFlip/screenSized/flipV） |
| `CNE_METAL_FBOLOG=1` | FBO 创建/失效日志 |
| `CNE_METAL_COVERAGE=1`（+`_OUT=<file>`） | 统计 269 个 `lime_gl_*` 入口的运行时调用次数 |
| `CNE_METAL_AOT_AUDIT=1` / `CNE_METAL_MTL_AUDIT=1` | AOT 命中率、shader 加载来源（metallib/source）与编译耗时 |
| `CNE_METAL_STATE_RESET=1` | 进入新状态时自动重建交换链/FBO（调试用，默认关闭） |
| `CNE_METAL_RT_UVFLIP`、`CNE_METAL_FBO_FLIP` 等 | 方向/采样翻转的实验开关（默认值即为已验证配置，勿随意改） |

完整列表见 `metal/native` 中各处的 `std::getenv("CNE_METAL_...")` 与 `metal/README.md`。

---

## 9. 后端设计要点

1. **GL 形状兼容层**：OpenFL/Lime 仍按 GLES 调用 `lime_gl_*`；后端把状态机、VAO、常量属性、
   FBO 附件、纹理格式等全部映射到 bgfx，并把每次 draw 转成 `bgfx::submit`。
2. **原生快路径**：`cne_metal_*` 系列跳过 CFFI 与 GL 枚举翻译（Lime 补丁后默认使用）。
3. **AOT 着色器**：构建期 GLSL→MSL 翻译 + `.metallib` 预编译；运行时只做 hash 查表，
   避免冷启动/首次进入状态的 shader 编译卡顿。
4. **方向/镜像规则**：渲染到离屏目标时按需对矩阵做 Y 镜像补偿；屏幕合成按"该目标的实际渲染方向"
   决定是否翻 V。该规则由 `CNE_METAL_DRAWPROBE/UVLOG` 实测确定，开发细节见
   `CodenameEngine-1.0.1/metal/README.md`。
5. **FBO 缓存**：按 (GL FBO, 附件纹理) 组合缓存 bgfx FrameBuffer，支持 GL 的 ping-pong 语义；
   纹理重传时自动失效关联 FBO。
6. **性能**：索引/顶点缓冲使用 revision 键做 O(1) 缓存；纹理同尺寸重传走 `updateTexture2D` 原地更新；
   热路径容器复用。

---

## 10. 游戏内容与 Mod

- 引擎代码与自带资源（`assets/`、`art/`）随仓库提供；`export/`（构建产物）、`mods/`（除 readme）
  默认被忽略。
- 第三方 mod（例如 Dusttale 的 `dustin-standalone-cne-modpack.zip`）不包含在本仓库：
  下载后解压到 `CodenameEngine.app/Contents/Resources/mods/<mod-name>/`（注意游戏读取的是 app bundle 内副本，
  不是工程目录的 `mods/`）。
- 若要提交到 GitHub：`assets/art` 体积约 460 MB，建议使用 [Git LFS](https://git-lfs.com/) 或
  按需裁剪；不含 `export/`、`bgfx-master/`、`_local/`。

---

## 11. 已知限制 / Roadmap

参见 [docs/METAL-TODO.md](docs/METAL-TODO.md)，摘要：

- OpenFL 仍使用 GL 形状 API（`lime_gl_*`），后续可做 Metal-native `Context3D`（方案见
  `metal/audit/openfl-native.md`）；
- 5 个 shader stage 尚未在构建期生成 `.metallib`（其中 4 个 mod shader 需上游修复）；
- `DisplayObject.mask`（stencil 状态未接线）、`readPixels`（截图/读回）为条件性功能缺口；
- 3D/Stage3D 能力（3D 纹理、MRT、transform feedback、uniform block 等）未实现。

---

## 12. 常见问题

| 现象 | 处理 |
| --- | --- |
| `Failed to create Fragment shader` | shader 未被 AOT；检查 `metal/README.md` §AOT 与 `CNE_METAL_AOT_RUNTIME_TRANSLATE` |
| 构建后画面全黑/方向错 | 先确认没有设置 `CNE_METAL_*` 实验开关；清理 `export/.../obj` 后重建 |
| 改了 Haxe 不生效 | 必须重新 `haxelib run lime build mac`；只 `apply.sh --build` 只更新 C++ ndll |
| app 启动闪退 | 检查 `lime.ndll` 是否为 cpp 版：`apply.sh … --deploy` |
| 进入某状态卡一下 | 用 `CNE_METAL_PROFILE=1 CNE_METAL_HITCH=1` 定位；多为资源创建/GC |

---

## 13. Credits & License

- 上游引擎：**Codename Engine** 团队（Apache-2.0）。见 `CodenameEngine-1.0.1/LICENSE` 与上游 README credits。
- 渲染库：**[bgfx](https://github.com/bkaradzic/bgfx)** / [bx](https://github.com/bkaradzic/bx) / [bimg](https://github.com/bkaradzic/bimg)（BSD-2-Clause，Copyright © Branimir Karadžić）。
- Haxe 生态：Lime、OpenFL、HaxeFlixel、flxanimate、hxvlc 等，遵循各自许可证。
- 本仓库的 Metal 后端代码、脚本与文档：Apache-2.0（与上游工程一致）。

如果这个移植对你有帮助，欢迎提出 issue / PR；调试与设计细节请先读
`CodenameEngine-1.0.1/metal/README.md` 与 `docs/METAL-TODO.md`。
