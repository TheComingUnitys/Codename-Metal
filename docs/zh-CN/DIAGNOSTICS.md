# Metal 后端诊断/测试手册

**中文** | [English](../DIAGNOSTICS.md)

本文件把散落的环境变量收敛成**统一入口**，并说明每类输出怎么读。
历史环境变量全部保持兼容（脚本只是映射），已有启动脚本无需修改。

一键验证（不跑游戏）见 `./verify.sh`；像素回归见 `metal/autotests/README.md`。

> 下文命令默认在 `CodenameEngine-1.0.1/` 目录下执行（`diag.sh` 位于 `metal/scripts/`）。

---

## 1. 统一入口：`metal/scripts/diag.sh`

```sh
# 列出所有“稳定类别”（每个类别对应一个或多个历史环境变量）
./metal/scripts/diag.sh --list

# 只打印将设置的 env（可用于 eval / 写进脚本）
./metal/scripts/diag.sh --print stats,fbo

# 直接带诊断启动（-- 之后是引擎可执行文件）
./metal/scripts/diag.sh -c stats,profile,hitch -- <app>/Contents/MacOS/CodenameEngine

# 第 240 帧截图，同时采集第 300 帧的抓帧数据
./metal/scripts/diag.sh --shot 240:/tmp/stage --capture-frame 300 --capture-dir /tmp/cap \
  -- <app>/Contents/MacOS/CodenameEngine

# 合成按键（帧:SDL scancode[:修饰键]）
./metal/scripts/diag.sh --keys "120:40,180:40" -- <app>/Contents/MacOS/CodenameEngine
```

| 类别 | 对应环境变量 | 输出/用途 |
| --- | --- | --- |
| `stats` | `CNE_METAL_STATS=1` | 每 120 帧：present/frame、bgfxDraws、glDrawElements、submitDraw、backbuffer |
| `profile` | `CNE_METAL_PROFILE=1` | 每 240 帧：CPU 前端耗时（bind/submit/viewAlloc/flip/view/uniform/bgfxSubmit）与纹理上传 |
| `hitch` | `CNE_METAL_HITCH=1` | 报告 >34ms 的帧及该帧创建的 tex/fbo/program 数量 |
| `hitch-detail` | `CNE_METAL_HITCH_DETAIL=1` | 慢帧拆成 game/submit/present/rest 四段 |
| `draws` | `CNE_METAL_DRAWPROBE=1` | 逐 draw：program/fbo/tex/size/rt/m0/m5/m13 |
| `fbo` | `CNE_METAL_FBOLOG=1` | FBO 创建/失效日志 |
| `uv` | `CNE_METAL_UVLOG=1` | 屏幕合成翻转判定 |
| `tex` | `CNE_METAL_TEXLOG=1` | 纹理创建尺寸日志（定位“渲染成缩略图”类问题） |
| `coverage` | `CNE_METAL_COVERAGE=1` | 输出 `lime_gl_*` 原语实际调用覆盖；路径由 `CNE_METAL_COVERAGE_OUT` 指定 |
| `shader` | `CNE_METAL_SHADER_AUDIT=1` | GLSL→MSL 审计日志（可用 `CNE_METAL_SHADER_AUDIT_FRAME` 限定帧） |
| `aot` | `CNE_METAL_AOT_AUDIT=1` | AOT 缓存查找汇总（hits/misses/runtimeTranslations） |
| `mtl` | `CNE_METAL_MTL_AUDIT=1` | Metal shader/program 加载与编译统计 |
| `debug` | `CNE_METAL_DEBUG=1` | 后端通用调试输出 |

特殊参数：

- `--shot <frame>:<path>[,<frame>:<path>...]`：`CNE_METAL_SHOT_RAW`（多帧；像素回归用）；
- `--capture-frame N` / `--capture-dir DIR`：`CNE_METAL_CAPTURE_FRAME` / `CNE_METAL_CAPTURE_DIR`；
- `--keys <spec>`：`CNE_METAL_AUTOKEYS`（`帧:scancode[:mods]`，scancode 40=Return、82/81=Up/Down）。

---

## 2. 输出格式速查

### 2.1 帧统计（`stats`）

```text
[cne-stats] present=961 frame=961 bgfxDraws=9 glDrawArrays=0 glDrawElements=14899 submitDraw=14899 backbuffer=2498x1440
```

- `bgfxDraws`：本帧 bgfx submit 的 draw 次数；
- `glDrawElements/glDrawArrays`：GL 形状入口调用次数（应≈`submitDraw`）；
- `present`：present 计数；`backbuffer`：当前绘制目标尺寸。

### 2.2 CPU 前端耗时（`profile`）

```text
[cne-prof] f=240 draws/frame=48.0 frontEnd=812.3us/frame [bind=... submitAll=... viewAlloc=... flip=... view=... uniformApply=... bgfxSubmit=... misc=...] uniform=... texUpload=... texMB=0.12/frame
```

阈值回归：`python3 metal/tools/profile_check.py <log> --max-frontend-us 1500 --max-hitch-ms 60`
（也支持 `--max-draws`；`--selftest` 不需要 app）。

### 2.3 卡顿（`hitch` / `hitch-detail`）

```text
[cne-hitch] frame=540 dt=210.4ms texCreates=12 fboCreates=3 programsLinked=4 now=12000ms
[cne-hitch] summary frame=7200 worstFrame=210.4ms
```

### 2.4 着色器/AOT（`aot`+`mtl`+`shader`）

```text
[cne-aot] EXIT cache=... entries=116 metallibStages=113 hits=... misses=0 runtimeTranslations=0
[cne-mtl] EXIT shaders=44 metallib=42 sourceCompile=2 metallibLoadFailures=0 ... newLibraryWithSourceMs=...
```

`sourceCompile>0` 表示有 stage 走了运行时 MSL 编译（AOT 未覆盖），对应
`metal/audit/aot-expected.json` 的 allowed fallback；出现 allowlist 之外的失败时
`./verify.sh` 的 strict 审计会失败。

### 2.5 截图/原始帧（`--shot`）

写入 `<path>.raw`（无头 4B/px，BGRA）与 `<path>.txt`（size/pitch/format/bytes）。
比对工具：`metal/tools/frame_diff.py`；场景化回归：`metal/scripts/pixel-regression.sh --selftest`。

---

## 3. 帧录制与直连 Metal 回放（A/B 原型）

```sh
# 1) 录制第 240 帧（+ 同帧原始截图）
CNE_METAL_CAPTURE_FRAME=240 CNE_METAL_CAPTURE_DIR=/tmp/cne-capture \
  ./metal/scripts/diag.sh --shot 240:/tmp/cne-shot/frame -- <app>/Contents/MacOS/CodenameEngine

# 2) 构建直连 Metal 回放器
./metal/tools/native_replay/build.sh /tmp/native_replay

# 3) 回放并输出原始帧，与 bgfx 的结果逐像素比对
/tmp/native_replay /tmp/cne-capture/f240 /tmp/replay
python3 ./metal/tools/native_replay/diff.py /tmp/cne-shot/frame.raw /tmp/replay.raw
```

说明：录制内容包含每 draw 的状态、MSL、顶点/索引字节、uniform、纹理，回放器不使用 bgfx/lime_gl。
该工具同时是 §1.2（去 bgfx）的前置验证。

---

## 4. 高级/实验开关（可能改变渲染语义，勿用于日常）

| 开关 | 作用 |
| --- | --- |
| `CNE_METAL_NOBLEND=1` | 关闭混合（用于区分“alpha 为 0”与“没画”） |
| `CNE_METAL_TEX_UPDATE=1` | 试验：流式纹理走 `updateTexture2D`（曾导致全黑，默认关） |
| `CNE_METAL_TEX_BGRA=1` | 强制 RGBA 上传按 BGRA 存储（字节序排查用） |
| `CNE_METAL_FORCE_SOURCE_COMPILE=1` | 跳过 metallib，强制运行时编译（排查预编译库问题） |
| `CNE_METAL_AOT_RUNTIME_TRANSLATE=0` | 关闭运行时 GLSL 翻译兜底（验证 AOT 覆盖率） |
| `CNE_METAL_STATE_RESET=1` | 新 FBO 首次出现时做一次状态重置（排查跨状态残留） |
| `CNE_METAL_SKIP_PROGRAM=<ids>` / `SKIP_DRAW` / `SKIP_VIEW` | 定向跳过 draw/program/view（定位单个绘制） |
| `CNE_METAL_DUMP_WORLD` / `SHOW_WORLD_RT` / `SHOW_WORLDCORNER` | 世界 RT 的调试显示 |
| `CNE_METAL_UVLOG`/`FBO_FLIP`/`COMPOSITE_FLIP`/`FLIP_COMPOSITE` | 翻转判定实验组（正常路径已自动判定） |

### 4.1 bgfx 无法表达 / 已决策不实现的 GL 状态

以下状态现在都会**记录进 `MetalContext::state` 并只告警一次**（`ReportUnimplemented`），
不再静默忽略；决策是不实现（2D 内容不触发，3D 需求出现时再评估）：

| GL 状态 | 原因 / 当前行为 |
| --- | --- |
| `glBlendColor` | bgfx v167 无 blend-colour setter；常量混合因子回退为普通因子 |
| `glDepthRange` | bgfx 无 depth-range 参数；固定 [0,1] |
| `glPolygonOffset` | bgfx 状态无 depth-bias 项 |
| `glLineWidth` (>1) | Metal/bgfx 只栅格化 1px 线 |
| MSAA / `glSampleCoverage` | 未启用 MSAA，覆盖恒满 |
| 嵌套 mask 的 stencil write-mask | bgfx v167 的 stencil 字没有 write-mask 字段 |
| 未知 client 像素格式 | 回退 RGBA8 / 4 B/px 并告警（窄格式已展开） |
| 离屏 view id > 199/帧 | 复用 view 199 并告警一次 |

完整列表（约 70 个）：

```sh
grep -rho 'CNE_METAL_[A-Z0-9_]*' CodenameEngine-1.0.1/metal | sort -u
```

新增诊断时请：优先加进 `diag.sh` 的类别表并在此文档登记，避免再新增无文档开关。

---

## 5. 常用组合

| 目标 | 命令 |
| --- | --- |
| 帧率/绘制数是否正常 | `diag.sh -c stats -- <app>` |
| 掉帧/菜单卡顿 | `diag.sh -c hitch,hitch-detail -- <app>` |
| shader 是否都命中 AOT | `diag.sh -c aot,mtl -- <app>` |
| 黑屏/方向问题 | `diag.sh -c uv,fbo,draws -- <app>` |
| AOT 覆盖回归 | `./verify.sh`（内部 `gen_shader_aot.py --check --strict`） |
| 画面像素回归 | `metal/scripts/pixel-regression.sh --selftest` / 见 autotests README |
