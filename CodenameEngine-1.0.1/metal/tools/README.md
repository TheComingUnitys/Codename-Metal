# 诊断工具

两个独立的小程序。它们不参与引擎构建，只在排查渲染问题时用。

## winid.m — 列出窗口 / 按窗口 ID 截图

```sh
clang -fobjc-arc -framework Foundation -framework CoreGraphics -o winid winid.m
./winid              # 列出全部窗口: id  pid  owner  layer  bounds  title
./winid <pid>        # 只看某个进程
./winid Codename     # 按 owner 名过滤
screencapture -x -l <CGWindowID> /tmp/shot.png
```

`screencapture -l` 能在窗口被**完全遮挡**时仍然截到它，而且不需要辅助功能权限。
配合 `pgrep -f CodenameEngine` 拿 PID 最稳：

```sh
PID=$(pgrep -f "CodenameEngine.app/Contents/MacOS/CodenameEngine" | head -1)
ID=$(./winid $PID | awk '$5 ~ /1249x/ {print $1}' | head -1)   # 主窗口
screencapture -x -l "$ID" /tmp/game.png
```

**踩过的坑**：`kCGWindowListOptionOnScreenOnly` 对被遮挡的游戏窗口经常返回**空列表**，
看起来像"没有这个窗口"。工具已改用 `kCGWindowListOptionAll`；同一个进程里
`0x0` / `1470x33` / `64x64` 那些是 SDL 与输入法的辅助窗口，主窗口要看 bounds。

## cametaltest.m — 环境能力验证

最小程序：创建 `NSWindow` + `CAMetalLayer`，每帧清成**红色**并 present。

```sh
clang -fobjc-arc -framework Cocoa -framework Metal -framework QuartzCore -o cametaltest cametaltest.m
./cametaltest
```

- **屏幕变红** → 本机可以正常显示 CAMetalLayer，问题一定在引擎侧
- **仍然黑** → 是环境/显示权限问题，不必再怀疑代码

这个测试排除了持续多轮对"本机能否显示 Metal 内容"的怀疑：实测全屏红色
（约 213080 个采样点），因此引擎侧的问题才是真正的方向。

## 引擎内建开关（环境变量）

| 变量 | 作用 |
| --- | --- |
| `CNE_METAL_CLEAR_TEST=1` | 强制洋红清屏（R==B，看不出通道互换） |
| `CNE_METAL_CLEAR_TEST=RRGGBB` / `RRGGBBAA` | 强制成指定纯色。**判断 R↔B 是否互换必须用非对称颜色**（例如 `FF0000`）：红屏说明呈现链不换通道，蓝屏说明换 |
| `CNE_METAL_TEX_BGRA=1` | 强制所有 8bit 纹理上传按 BGRA8 建（诊断用兜底；正常应由 `EXT_bgra` 自动决定） |
| `CNE_METAL_NOBLEND=1` | 关闭混合，判别"全透明"与"没画" |
| `CNE_METAL_DEBUG=1` | 属性布局 / 程序属性表 / 顶点数据 dump / MSL 落盘 / `[cne-tex]` 纹理字节序统计 |
| `CNE_METAL_STATS=1` | `bgfxDraws` / `glDrawArrays` / `glDrawElements` 计数 |
| `CNE_METAL_PRESENT_TEST=1` | 用自填纹理走完 blit+present，绕过 bgfx 的渲染输出 |
| `CNE_METAL_SHOT=帧:路径` | 引擎内截图（读后台缓冲，不需录屏权限；当前 bgfx/Metal 截图路径不落盘，改用 winid + screencapture） |

`[cne-tex]` 的例子（`CNE_METAL_DEBUG=1`）：

```
[cne-tex] 1843x1396 internalFormat=0x1908 format=0x80E1 type=0x1401 -> BGRA8
          opaque=65536 avgR=119.9 avgB=147.7 avgA=250.8  R>B in 12261
```

把 `avgR/avgB` 和素材 PNG 的 raw / premultiplied / premultiplied-R↔B-swapped 三种
假设逐通道比对，就能确认上传字节到底是哪一种（见 README 第 49 节）。

## 音频是否真的在输出：CoreAudio process object

macOS 14.2+ 可以直接问系统"某个进程在往音频设备渲染吗"（`/tmp/audioproc.m`）：

```sh
clang -fobjc-arc -framework Foundation -framework CoreAudio -o audioproc audioproc.m
./audioproc <pid>        # 不带参数则列出所有音频进程
```

```
obj=186 pid=88067 runningInput=0 runningOutput=1 bundle=com.yoshman29.codenameengine
```

`runningOutput=1` 才算真的在发声。对照：`afplay /System/Library/Sounds/Ping.aiff &` 后
查它的 pid 应该是 1，`runningOutput=0` 说明没在渲染。

配合 OpenAL Soft 自己的日志开关，一眼看出用的是哪个后端：

```sh
ALSOFT_LOGLEVEL=3 ALSOFT_LOGFILE=/tmp/alsoft.log ./CodenameEngine
grep -E "Supported backends|Initialized backend|Created device" /tmp/alsoft.log
# 正常应该是： Supported backends: core, null / Created device …, "MacBook Air Speakers"
```

`Supported backends: null` + `Created device …, "No Output"` = 所有声音都进了虚空。

## 自动按键：`CNE_METAL_AUTOKEYS`（无需任何系统权限）

```sh
CNE_METAL_AUTOKEYS="1300:40,1700:81,1850:81,2050:40,2600:40" ./CodenameEngine
#                     帧号:按键, …      回车=40 上=82 下=81 左=80 右=79 Esc=41 Tab=43
```

用 `SDL_PushEvent` 往 lime 自己的事件队列里塞键，绕开"没有辅助功能权限就
没法从外部按键"的限制。**KEYDOWN 和 KEYUP 必须跨帧**（保持 4 帧），
否则 Flixel 在一次 update 前收到按下+抬起，`justPressed` 不会触发。

配合 `open -a CodenameEngine.app`（把窗口激活）与
`screencapture -x -l <窗口ID>`（Harness 窗口浮在最上面也能截到游戏）

## 视图排序模式 A/B：`CNE_METAL_VIEWMODE`

```sh
CNE_METAL_VIEWMODE=default ./CodenameEngine     # bgfx 默认（按 program/blend 排序）
CNE_METAL_VIEWMODE=depth   ./CodenameEngine     # DepthAscending
# 不设 = Sequential（按提交顺序，2D 渲染器需要的那个）
```

启动时会打印实际用的模式：`[cne-metal] view mode = 1 (env=unset)`。

## 绘制顺序探针

`CNE_METAL_DEBUG=1` 时每一笔都会打印帧号、序号和首个顶点坐标：

```
[cne-dbg] f=2555 draw#11 stream 0 stride=20 bytes=224 base=0 pos=(95.0,65.0) layout: …
```

用来判断"谁先被提交"，比看画面推断可靠。

## 构建期工具：`CompileShaderAot.mm` —— MSL → `.metallib`

```sh
# 平时不需要手动跑：apply.sh -> gen_shader_aot.py 会自动构建并调用它
python3 metal/scripts/gen_shader_aot.py            # 增量（shader 不变时不重编）

# 手动：把 manifest 里每个 stage 编成 outdir/<key>.metallib + .meta
clang++ -x objective-c++ -fobjc-arc -std=c++17 -O2 \
  metal/tools/CompileShaderAot.mm -framework Metal -framework Foundation \
  -o metal/tools/build/CompileShaderAot
metal/tools/build/CompileShaderAot <manifest.json> <outdir> [--force]
```

- **本机没有完整 Xcode / `xcrun metal`**，所以用系统自带的 Metal 编译器
  （`newLibraryWithSource` 在**构建期进程**里驱动），再用 `_MTLLibrary` 的私有
  `serializeToURL:error:` 落盘成真正的 `.metallib`（`file` 认出 `MetalLib executable`）。
  游戏内只用公开的 `newLibraryWithData` 加载，私有 API 不出现在运行时。
- 每个产物都**当场回载验证**：`newLibraryWithData` + `newFunctionWithName:@"xlatMtlMain"`。
- 增量：`<key>.meta` 记录 `MSL 指纹 + 设备/OS 标签 + 字节数`，三者一致才复用；
  换机器 / 升系统会自动重编。
- 私有 API 不可用（未来 macOS 移除）时退出码 4 → 生成器降级为「只用 MSL」，构建照常。
- 某个 stage 的 MSL 本身编译不过时只跳过该 stage（运行时回退 `newLibraryWithSource`），
  不会让构建失败。

产物目录：`metal/generated/metallibs/<macOS版本>_<build>_<arch>/`（本机构建缓存）。
